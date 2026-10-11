#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/onenet_token_check.py —— 在 PC 上验证 OneNET 设备鉴权 token（不需要烧板子）

为什么需要这个脚本
--------------------------------------------------------------------------
阶段 4 的失败症状全都长得一样：「连不上 OneNET」。但成因至少有两类：
    (a) Wi-Fi 没连上（密码错、5GHz、AP 白名单）
    (b) Wi-Fi 连上了，但 OneNET 拒绝 MQTT 登录（产品 ID / 设备名 / 设备密钥 / token 算法）
只有串口日志的话，这两类要一轮轮烧板子试。本脚本在 PC 上直接跑 (b)，
一次就把责任面切开。

它做的事
--------------------------------------------------------------------------
1. 用与 firmware/components/onenet/onenet_token.c **完全相同的算法**算 token；
2. 拿这个 token 向 studio-mqtts.heclouds.com:8883 发一次真实的 MQTT CONNECT（TLS）；
3. 打印 CONNACK 返回码：
       0  连接成功 —— 三元组与算法都正确
       1  协议版本不支持
       4  用户名或密码错误 —— token 或凭据有问题（最常见）
       5  未授权 —— 产品/设备状态不对，或设备已被别的连接顶掉

CA 证书直接从 onenet.c 里抽出来用，不在这里另存一份 —— 避免以后换了证书两处不同步。

只依赖 Python 标准库：ssl / socket / hmac / hashlib / base64 / struct / re / argparse。

用法
--------------------------------------------------------------------------
    python tools/onenet_token_check.py
    python tools/onenet_token_check.py --secrets firmware/main/app_secrets.h
    python tools/onenet_token_check.py --json      # 只打印 token，不发连接

【安全提醒】token 等同于设备密码。--json 会把整串打出来，别把它贴到公开的地方。
"""

import argparse
import base64
import hashlib
import hmac
import json
import os
import re
import socket
import ssl
import struct
import sys

# 与 onenet.h 保持一致
MQTT_HOST = "studio-mqtts.heclouds.com"
MQTT_PORT = 8883
TOKEN_VERSION = "2018-10-31"
TOKEN_EXPIRE_EPOCH = 2524608000   # ≈ 2050-01-01，与 ONENET_TOKEN_EXPIRE_EPOCH 相同

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SECRETS = os.path.join(REPO_ROOT, "firmware", "main", "app_secrets.h")
DEFAULT_ONENET_C = os.path.join(REPO_ROOT, "firmware", "components", "onenet", "onenet.c")


# ===========================================================================
# 凭据与证书的提取
# ===========================================================================

def load_secrets(path):
    """从 app_secrets.h 里抓 #define NAME "value"。"""
    with open(path, "r", encoding="utf-8", errors="replace") as fp:
        text = fp.read()

    wanted = ("ONENET_PRODUCT_ID", "ONENET_DEVICE_NAME", "ONENET_DEVICE_KEY",
              "WIFI_SSID", "WIFI_PASSWORD")
    found = {}
    for name in wanted:
        m = re.search(r'^\s*#define\s+%s\s+"([^"]*)"' % re.escape(name), text, re.MULTILINE)
        found[name] = m.group(1) if m else None

    missing = [n for n in ("ONENET_PRODUCT_ID", "ONENET_DEVICE_NAME", "ONENET_DEVICE_KEY")
               if not found.get(n)]
    if missing:
        raise SystemExit("[错误] %s 里缺少或未填写：%s" % (path, "、".join(missing)))
    return found


def extract_ca_pem(onenet_c_path):
    """把 onenet.c 里 ONENET_CA_CERT[] 的 C 字符串字面量拼回 PEM 文本。"""
    with open(onenet_c_path, "r", encoding="utf-8", errors="replace") as fp:
        text = fp.read()

    start = text.find("ONENET_CA_CERT[]")
    if start < 0:
        raise SystemExit("[错误] 在 %s 里找不到 ONENET_CA_CERT" % onenet_c_path)
    end = text.find(";", start)
    block = text[start:end]

    # 逐个取出 "..." 字面量并还原 \n
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', block)
    if not parts:
        raise SystemExit("[错误] ONENET_CA_CERT 里没解析出任何字符串字面量")
    pem = "".join(parts).replace("\\n", "\n").replace('\\"', '"')
    if "BEGIN CERTIFICATE" not in pem:
        raise SystemExit("[错误] 拼出来的内容不像 PEM 证书")
    return pem


# ===========================================================================
# token 算法 —— 必须与 onenet_token.c 逐行对应
# ===========================================================================

def url_encode_byte(b):
    """与 onenet_token.c 的 url_encode_byte() 一致：只编码会破坏 query string 的字符。"""
    special = {
        ord('+'): "%2B", ord(' '): "%20", ord('/'): "%2F", ord('?'): "%3F",
        ord('%'): "%25", ord('#'): "%23", ord('&'): "%26", ord('='): "%3D",
    }
    if b in special:
        return special[b]
    return chr(b)


def generate_token(product_id, device_name, device_key,
                   expire_time=TOKEN_EXPIRE_EPOCH, method="sha256"):
    """对应 onenet_token.c 的 onenet_token_generate()。"""
    # 1. 待签字符串
    sig_text = "%u\n%s\nproducts/%s/devices/%s\n%s" % (
        expire_time, method, product_id, device_name, TOKEN_VERSION)

    # 2. Base64 解码设备密钥（add padding 容错：控制台里复制的密钥有时少了 '=' 填充）
    key_b64 = device_key.strip()
    key_b64 += "=" * (-len(key_b64) % 4)
    key = base64.b64decode(key_b64)

    # 3. HMAC 签名
    digestmod = hashlib.sha256 if method == "sha256" else hashlib.sha1
    mac = hmac.new(key, sig_text.encode("utf-8"), digestmod).digest()

    # 4. Base64 编码签名（不带换行），再逐字节 URL 编码
    sign_b64 = base64.b64encode(mac).decode("ascii")
    sign_enc = "".join(url_encode_byte(ord(c)) for c in sign_b64)

    # 5. 拼 token
    return ("version=%s&res=products%%2F%s%%2Fdevices%%2F%s&et=%u&method=%s&sign=%s"
            % (TOKEN_VERSION, product_id, device_name, expire_time, method, sign_enc)), sig_text


# ===========================================================================
# 最小 MQTT CONNECT（只为了看一眼 CONNACK 返回码）
# ===========================================================================

def _mqtt_str(s):
    data = s.encode("utf-8")
    return struct.pack(">H", len(data)) + data


def build_connect(client_id, username, password, keepalive=60):
    payload = _mqtt_str(client_id) + _mqtt_str(username) + _mqtt_str(password)

    # 连接标志：clean session(0x02) | password(0x40) | username(0x80)
    flags = 0x02 | 0x40 | 0x80
    variable = _mqtt_str("MQTT") + bytes([0x04, flags]) + struct.pack(">H", keepalive)

    body = variable + payload
    return b"\x10" + _encode_remaining_length(len(body)) + body


def _encode_remaining_length(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n > 0:
            b |= 0x80
        out.append(b)
        if n == 0:
            return bytes(out)


def _cert_names(der):
    """从 DER 证书里取出 (SAN DNS 列表, CN, 完整解码结果)。"""
    import tempfile
    pem = ssl.DER_cert_to_PEM_cert(der)
    path = os.path.join(tempfile.gettempdir(), "onenet_peer_probe.pem")
    with open(path, "w", encoding="ascii") as fp:
        fp.write(pem)
    try:
        info = ssl._ssl._test_decode_cert(path)     # CPython 私有 API，但没有第三方依赖
    finally:
        try:
            os.remove(path)
        except OSError:
            pass

    san = [v for k, v in info.get("subjectAltName", ()) if str(k).lower() == "dns"]
    cn = ""
    for rdn in info.get("subject", ()):
        for key, val in rdn:
            if key == "commonName":
                cn = val
    return san, cn, info


def _name_matches(pattern, host):
    """极简主机名匹配：精确或最左一段通配。"""
    pattern = pattern.lower().rstrip(".")
    host = host.lower().rstrip(".")
    if pattern == host:
        return True
    if pattern.startswith("*."):
        return host.count(".") == pattern.count(".") and host.endswith(pattern[1:])
    return False


def check_server_identity(der, host):
    """
    按 **mbedTLS 的语义**校验服务器身份（这是本脚本第一次跑报错的根因所在）。

    OneNET 的 MQTTS 证书是自签的，而且**没有 subjectAltName 扩展**，
    只有 CN = "OneNET MQTTS"。Python 3.7 起彻底取消了"无 SAN 时回退比 CN"的行为
    （OpenSSL 的 X509_CHECK_FLAG_NEVER_CHECK_SUBJECT），所以 Python 必然报
    hostname mismatch；而 ESP-IDF 用的 mbedTLS 在无 SAN 时**是会回退到 CN** 的。
    也就是说：同样的证书，Python 判不合格、固件判合格。

    所以这里手工复现 mbedTLS 的规则：有 SAN 就用 SAN，没 SAN 就比 CN。
    证书链本身仍然由 load_verify_locations() 严格校验（固定到 onenet.c 里内嵌的 CA）。
    """
    san, cn, info = _cert_names(der)
    if san:
        ok = any(_name_matches(p, host) for p in san)
        detail = "subjectAltName = %s" % ", ".join(san)
        rule = "按 SAN 比对"
    else:
        ok = _name_matches(cn, host)
        detail = "CN = %r（证书没有 SAN 扩展）" % cn
        rule = "无 SAN，按 CN 比对（mbedTLS 语义）"
    return ok, detail, rule, info


CONNACK_TEXT = {
    0: "连接成功",
    1: "协议版本不被支持",
    2: "client_id 不合法",
    3: "服务端不可用",
    4: "用户名或密码错误（token / 凭据问题）",
    5: "未授权（产品/设备状态不对，或已被顶下线）",
}


def try_connect(ca_pem, username, password, client_id, timeout=15, strict_hostname=False):
    """连上并读 CONNACK。返回 dict：connack / error / identity。"""
    result = {"connack": None, "error": None, "identity": None, "tls_version": None}

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    # check_hostname 必须关掉：见 check_server_identity() 的说明 —— Python 做不到
    # mbedTLS 的"无 SAN 回退 CN"，开着它就永远连不上 OneNET。
    ctx.check_hostname = bool(strict_hostname)
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_verify_locations(cadata=ca_pem)

    try:
        with socket.create_connection((MQTT_HOST, MQTT_PORT), timeout=timeout) as raw:
            with ctx.wrap_socket(raw, server_hostname=MQTT_HOST) as tls:
                result["tls_version"] = tls.version()
                der = tls.getpeercert(binary_form=True)
                ok, detail, rule, _info = check_server_identity(der, MQTT_HOST)
                result["identity"] = {"ok": ok, "detail": detail, "rule": rule}

                tls.sendall(build_connect(client_id, username, password))
                head = tls.recv(2)
                if len(head) < 2:
                    result["error"] = "服务端在 CONNACK 之前就关掉了连接"
                    return result
                if head[0] != 0x20:
                    result["error"] = ("收到意料之外的报文类型 0x%02X（期望 0x20 CONNACK）"
                                       % head[0])
                    return result
                rest = tls.recv(2)
                if len(rest) < 2:
                    result["error"] = "CONNACK 只收到一半"
                    return result
                result["connack"] = rest[1]
                return result
    except ssl.SSLCertVerificationError as exc:
        result["error"] = "TLS_CERT: %s" % exc
    except (socket.timeout, TimeoutError):
        result["error"] = "TIMEOUT"
    except OSError as exc:
        result["error"] = "OSERROR: %s" % exc
    return result


# ===========================================================================
# main
# ===========================================================================

def main():
    ap = argparse.ArgumentParser(description="在 PC 上验证 OneNET 设备鉴权 token")
    ap.add_argument("--secrets", default=DEFAULT_SECRETS, help="app_secrets.h 路径")
    ap.add_argument("--onenet-c", default=DEFAULT_ONENET_C, help="onenet.c 路径（取 CA 证书）")
    ap.add_argument("--method", default="sha256", choices=["sha256", "sha1"])
    ap.add_argument("--json", action="store_true", help="只打印 token 与中间结果，不连服务器")
    ap.add_argument("--timeout", type=float, default=15.0, help="建连超时（秒）")
    ap.add_argument("--strict-hostname", action="store_true",
                    help="用 Python 默认的严格 SAN 校验（对 OneNET 必然失败，仅用于演示该差异）")
    args = ap.parse_args()

    if not os.path.isfile(args.secrets):
        raise SystemExit("[错误] 找不到 %s" % args.secrets)

    s = load_secrets(args.secrets)
    pid, dev, key = s["ONENET_PRODUCT_ID"], s["ONENET_DEVICE_NAME"], s["ONENET_DEVICE_KEY"]

    print("=" * 68)
    print("OneNET 设备鉴权 token 自检")
    print("=" * 68)
    print("凭据文件 : %s" % args.secrets)
    print("产品 ID  : %s" % pid)
    print("设备名   : %s" % dev)
    print("设备密钥 : %s（%d 字符）" % (key[:4] + "…" + key[-2:] if len(key) > 8 else "…", len(key)))

    try:
        token, sig_text = generate_token(pid, dev, key, method=args.method)
    except Exception as exc:
        raise SystemExit("[错误] token 生成失败：%s\n"
                         "       最常见成因：设备密钥不是合法 Base64（复制时漏了尾巴或混进空格）" % exc)

    print("-" * 68)
    print("待签字符串（换行用 \\n 显示）：")
    print("  %s" % sig_text.replace("\n", "\\n"))
    print("token 长度: %d 字符" % len(token))
    # 只打印 &sign= 之前的部分 —— 签名那段等同于设备密码，不要落到日志/对话里
    print("token 前缀: %s&sign=<已隐藏>" % token.split("&sign=")[0])
    print("-" * 68)

    if args.json:
        print(json.dumps({"token": token, "sig_text": sig_text,
                          "product_id": pid, "device_name": dev},
                         ensure_ascii=False, indent=2))
        return 0

    try:
        ca_pem = extract_ca_pem(args.onenet_c)
    except SystemExit as exc:
        print(exc)
        return 2

    print("正在连接 %s:%d （TLS，证书链固定到 %s）……"
          % (MQTT_HOST, MQTT_PORT, os.path.basename(args.onenet_c)))
    r = try_connect(ca_pem, pid, token, dev, timeout=args.timeout,
                    strict_hostname=args.strict_hostname)

    if r.get("tls_version"):
        print("   TLS 版本：%s" % r["tls_version"])

    ident = r.get("identity")
    if ident:
        mark = "✅" if ident["ok"] else "⚠️"
        print("%s 服务器身份：%s —— %s" % (mark, ident["rule"], ident["detail"]))
        if not ident["ok"]:
            print()
            print("   【这是一个必须知道的结论，不是本机的问题】")
            print("   OneNET 的 MQTTS 证书是**自签的、且完全没有 SAN 扩展**，")
            print("   证书上写的是 CN=OneNET MQTTS，而不是 %s。" % MQTT_HOST)
            print("   而 mbedTLS 在证书无 SAN 时会回退比 CN（x509_crt.c:2991-3005），")
            print("   不符就置 MBEDTLS_X509_BADCERT_CN_MISMATCH；")
            print("   esp-tls 只要 mbedtls_ssl_get_verify_result() != 0 就判握手失败")
            print("   （esp_tls_mbedtls.c:351）。")
            print("   → 所以固件里必须设 skip_cert_common_name_check = true，")
            print("     否则永远卡在 TLS，走不到 MQTT。信任是由内嵌的 CA 钉住的（证书链仍严格校验），")
            print("     CN 这一项在这里不提供任何额外安全性，只会挡路。")
            print()

    err = r.get("error")
    if isinstance(err, str) and err.startswith("TLS_CERT:"):
        print("❌ TLS 证书链验证失败：%s" % err[len("TLS_CERT:"):].strip())
        print("   → onenet.c 里内嵌的 CA 与服务器当前证书不匹配了。")
        print("     这不是凭据问题，是需要换 onenet.c 里的 PEM。")
        return 3
    if err == "TIMEOUT":
        print("❌ 连接超时（%.0f 秒）。" % args.timeout)
        print("   → 本机到 %s:8883 的出站被挡住了。" % MQTT_HOST)
        print("   → 注意：git 走本机代理（见 README），但 Python 的 socket 不走 WinINET")
        print("     代理，是直连的。")
        print("   → 本项失败【不能】说明凭据有问题，只是这条路走不通。")
        return 4
    if isinstance(err, str) and err.startswith("OSERROR:"):
        print("❌ 连接失败：%s" % err[len("OSERROR:"):].strip())
        print("   → 同上：本项失败不能说明凭据有问题。")
        return 4
    if err:
        print("❌ %s" % err)
        return 5

    code = r["connack"]
    text = CONNACK_TEXT.get(code, "未知返回码")
    print("CONNACK 返回码 = %d ：%s" % (code, text))

    if code == 0:
        print()
        print("✅ 结论：产品 ID / 设备名 / 设备密钥三元组正确，token 算法正确。")
        print("   既然 PC 能连上，板子连不上就只剩两类原因：")
        print("     ① Wi-Fi 没连上（看串口日志有没有「拿到 IP」）")
        print("     ② 板子上的 token 算错了（对照本脚本打印的待签字符串逐字核对）")
        return 0

    print()
    print("❌ 结论：服务端拒绝了这次登录，问题在凭据或 token 本身，与 Wi-Fi / 固件无关。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
