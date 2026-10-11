#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/onenet_auth_probe.py —— OneNET 鉴权失败时的"责任面"排查器

背景
--------------------------------------------------------------------------
2026-10-11 用 tools/onenet_token_check.py 复现出的结果：
  · TLS 握手成功（说明 onenet.c 里内嵌的 CA 完全正确，证书链能验过）
  · 服务器返回 CONNACK = 4「用户名或密码错误」
于是问题被压缩到"凭据 / token"这一小块。但这一小块里仍有好几种可能：

  a) 设备密钥本身抄错了（最常见）
  b) 设备密钥需要 base64 解码后再当 HMAC 密钥（官方文档这么写）
  c) 签名方法不是 sha256
  d) 产品 ID / 设备名与平台上的对不上
  e) 设备建在了另一个平台实例上，对应另一个接入点

本脚本把 (b)(c)(e) 这几类**可以自动枚举**的假设一次性试完，
试完还全是 4，那嫌疑就只剩 (a)(d) —— 需要人去控制台核对，
而不是继续在代码里瞎改。

注意：每试一个变体就要建一次 TLS 连接，一次约 1 秒。变体数量有限，
不会触发平台的频控；但也不要写循环去暴力枚举密钥。

用法
--------------------------------------------------------------------------
    python tools/onenet_auth_probe.py
    python tools/onenet_auth_probe.py --secrets firmware/main/app_secrets.h

输出每一行是一个变体的 HOST/密钥处理/签名方法 与 CONNACK 返回码。
"""

import argparse
import base64
import hashlib
import hmac
import os
import socket
import ssl
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import onenet_token_check as T   # noqa: E402  复用 token 算法与凭据读取

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SECRETS = os.path.join(REPO_ROOT, "firmware", "main", "app_secrets.h")
DEFAULT_ONENET_C = os.path.join(REPO_ROOT, "firmware", "components", "onenet", "onenet.c")

# 各接入点。studio-mqtts 是 OneNET Studio（物模型）用的；mqtts 是旧版多协议接入。
HOSTS = {
    "studio": ("studio-mqtts.heclouds.com", 8883),
    "old": ("mqtts.heclouds.com", 8883),
}

DIGESTS = {"sha256": hashlib.sha256, "sha1": hashlib.sha1, "md5": hashlib.md5}


def build_token(pid, dev, key_bytes, method, et):
    """按官方 StringForSignature = et\\nmethod\\nres\\nversion 生成 token。"""
    res = "products/%s/devices/%s" % (pid, dev)
    sig_text = "%u\n%s\n%s\n%s" % (et, method, res, T.TOKEN_VERSION)
    mac = hmac.new(key_bytes, sig_text.encode("utf-8"), DIGESTS[method]).digest()
    sign_b64 = base64.b64encode(mac).decode("ascii")
    sign_enc = "".join(T.url_encode_byte(ord(c)) for c in sign_b64)
    return ("version=%s&res=products%%2F%s%%2Fdevices%%2F%s&et=%u&method=%s&sign=%s"
            % (T.TOKEN_VERSION, pid, dev, et, method, sign_enc))


def attempt(ca_pem, host, port, username, password, client_id, timeout=12):
    """返回 (connack 或 None, 说明文字)。"""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False          # 见 onenet_token_check.check_server_identity 的说明
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_verify_locations(cadata=ca_pem)

    try:
        with socket.create_connection((host, port), timeout=timeout) as raw:
            with ctx.wrap_socket(raw, server_hostname=host) as tls:
                tls.sendall(T.build_connect(client_id, username, password))
                head = tls.recv(2)
                if len(head) < 2:
                    return None, "服务端未回 CONNACK 就断开"
                if head[0] != 0x20:
                    return None, "报文类型 0x%02X（非 CONNACK）" % head[0]
                rest = tls.recv(2)
                if len(rest) < 2:
                    return None, "CONNACK 不完整"
                return rest[1], ""
    except ssl.SSLCertVerificationError as exc:
        return None, "TLS 证书链失败：%s" % exc
    except (socket.timeout, TimeoutError):
        return None, "超时"
    except OSError as exc:
        return None, "连接错误：%s" % exc


def main():
    ap = argparse.ArgumentParser(description="OneNET 鉴权变体排查")
    ap.add_argument("--secrets", default=DEFAULT_SECRETS)
    ap.add_argument("--onenet-c", default=DEFAULT_ONENET_C)
    ap.add_argument("--timeout", type=float, default=12.0)
    args = ap.parse_args()

    s = T.load_secrets(args.secrets)
    pid, dev, key_str = s["ONENET_PRODUCT_ID"], s["ONENET_DEVICE_NAME"], s["ONENET_DEVICE_KEY"]
    ca_pem = T.extract_ca_pem(args.onenet_c)

    padded = key_str.strip() + "=" * (-len(key_str.strip()) % 4)
    try:
        key_decoded = base64.b64decode(padded)
        decoded_note = "base64 解码成 %d 字节" % len(key_decoded)
    except Exception as exc:
        key_decoded = None
        decoded_note = "**不是合法 base64**：%s" % exc
    key_raw = key_str.encode("utf-8")

    import time
    et_far = T.TOKEN_EXPIRE_EPOCH
    et_near = int(time.time()) + 365 * 86400

    print("=" * 78)
    print("OneNET 鉴权变体排查")
    print("=" * 78)
    print("产品 ID   : %s" % pid)
    print("设备名    : %s" % dev)
    print("设备密钥  : %s…（%d 字符，%s）" % (key_str[:4], len(key_str), decoded_note))
    if key_decoded is not None:
        printable = all(32 <= b < 127 for b in key_decoded)
        print("            解码后%s可打印 ASCII" % ("" if printable else "**不**"))
    print("-" * 78)

    variants = []
    if key_decoded is not None:
        for method in ("sha256", "sha1", "md5"):
            variants.append(("studio / 解码密钥 / %s / et远期" % method, "studio",
                             pid, build_token(pid, dev, key_decoded, method, et_far), dev))
        variants.append(("studio / 解码密钥 / sha256 / et=+1年", "studio",
                         pid, build_token(pid, dev, key_decoded, "sha256", et_near), dev))
        variants.append(("old / 解码密钥 / sha256", "old",
                         pid, build_token(pid, dev, key_decoded, "sha256", et_far), dev))
    variants.append(("studio / 原始密钥字节 / sha256", "studio",
                     pid, build_token(pid, dev, key_raw, "sha256", et_far), dev))
    variants.append(("studio / 明文密钥当密码（不用 token）", "studio",
                     pid, key_str, dev))
    variants.append(("studio / 解码后明文当密码", "studio",
                     pid, key_decoded.decode("ascii", "replace") if key_decoded else key_str, dev))

    for label, host_key, username, password, client_id in variants:
        host, port = HOSTS[host_key]
        code, note = attempt(ca_pem, host, port, username, password, client_id,
                             timeout=args.timeout)
        if code is None:
            print("%-42s %-22s → %s" % (label, host, note))
        else:
            mark = "✅" if code == 0 else "  "
            print("%-42s %-22s → %s CONNACK=%d %s"
                  % (label, host, mark, code, T.CONNACK_TEXT.get(code, "")))

    print("-" * 78)
    print("若全部为 4：签名算法与接入点都不是原因，")
    print("请到 OneNET 控制台核对【产品ID / 设备名 / 设备密钥】三者，")
    print("尤其是设备密钥是否按了「显示」后完整复制（含结尾的 '='）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
