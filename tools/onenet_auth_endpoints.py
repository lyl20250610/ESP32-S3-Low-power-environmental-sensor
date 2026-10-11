#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OneNET —— 换接入点 + 换鉴权形态的排查。

为什么想到这一步
----------------
项目里的设备密钥长这样：

    NVFhUFl0TTlOWmxRUEVGSFR0YWd1ZXN1VXFQNEhONVI=

把它 base64 解码，得到的是 **32 个可打印 ASCII 字符**：
`5QaPYtM9NZlQPEFHTtaguesuUqP4HN5R`。

"base64(一段 32 字符随机 ASCII)" 正是 **旧版 OneNET（多协议接入 /
MQTT物联网套件）的 API Key（鉴权信息）** 格式。新版 OneNET Studio 的设备密钥
一般是直接给一串随机字符串，不是这种"base64 套一层"的结构。

而旧版平台的连接方式和新版**完全不同**：

| | 旧版（多协议接入） | 新版（Studio） |
|---|---|---|
| 主机/端口 | `mqtt.heclouds.com:6002`（明文） | `studio-mqtts.heclouds.com:8883`（TLS） |
| clientId | **设备ID**（长数字串） | 设备名称 |
| username | 产品ID | 产品ID |
| password | **API Key 原文，直接用** | 鉴权 token |

之前 20 个变体**只测过 TLS 的 8883**，而且是"password = token"这一种思路。
"明文端口 + password 就是密钥原文"这一整块从来没测过。

所以这一轮测：
  1. 先把候选 (主机, 端口) 全部做一次**连通性探测**——连不上的直接跳过。
  2. 能连上的，再扫 4 种 password 形态 × 明文/TLS。

用法
----
    python tools/onenet_auth_endpoints.py
"""

import argparse
import base64
import hashlib
import hmac
import os
import socket
import ssl
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import onenet_token_check as T  # noqa: E402

# (主机, 端口, 是否 TLS)
ENDPOINTS = [
    ("studio-mqtts.heclouds.com", 8883, True),    # 新版 TLS（已知可达，基线）
    ("studio-mqtts.heclouds.com", 1883, True),    # 试探
    ("studio-mqtt.heclouds.com", 1883, False),    # 新版明文
    ("mqtts.heclouds.com", 8883, True),           # 旧版 TLS
    ("mqtt.heclouds.com", 1883, False),           # 旧版明文
    ("mqtt.heclouds.com", 6002, False),           # 旧版多协议接入 MQTT 端口
    ("mqtt.heclouds.com", 6002, True),
    ("mqtts.heclouds.com", 6002, True),
    ("iot-mqtts.heclouds.com", 8883, True),       # 试探
]


def probe_tcp(host, port, timeout=4):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError as exc:
        return False, str(exc)


def connack(host, port, use_tls, ca_pem, client_id, username, password, timeout=8):
    """返回 (connack 或 None, 说明)。"""
    try:
        raw = socket.create_connection((host, port), timeout=timeout)
    except OSError as exc:
        return None, "TCP 连不上：%s" % exc

    try:
        if use_tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False       # 见 onenet_token_check.check_server_identity
            ctx.verify_mode = ssl.CERT_REQUIRED
            ctx.load_verify_locations(cadata=ca_pem)
            try:
                sock = ctx.wrap_socket(raw, server_hostname=host)
            except ssl.SSLError as exc:
                raw.close()
                return None, "TLS 失败：%s" % exc
        else:
            sock = raw

        with sock:
            sock.sendall(T.build_connect(client_id, username, password))
            sock.settimeout(timeout)
            head = sock.recv(2)
            if len(head) < 2:
                return None, "未回 CONNACK 就断开"
            if head[0] != 0x20:
                return None, "报文类型 0x%02X（非 CONNACK）" % head[0]
            rest = sock.recv(2)
            if len(rest) < 2:
                return None, "CONNACK 不完整"
            return rest[1], ""
    except (socket.timeout, TimeoutError):
        return None, "超时"
    except OSError as exc:
        return None, "连接错误：%s" % exc


def main():
    ap = argparse.ArgumentParser(description="OneNET 接入点与鉴权形态排查")
    ap.add_argument("--secrets", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "firmware", "main", "app_secrets.h"))
    ap.add_argument("--onenet-c", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "firmware", "components", "onenet", "onenet.c"))
    args = ap.parse_args()

    secrets = T.load_secrets(args.secrets)
    pid = secrets["ONENET_PRODUCT_ID"]
    dev = secrets["ONENET_DEVICE_NAME"]
    key_raw = secrets["ONENET_DEVICE_KEY"].strip()
    key_b64 = key_raw + "=" * (-len(key_raw) % 4)
    key_dec = base64.b64decode(key_b64)
    ca_pem = T.extract_ca_pem(args.onenet_c)

    print("=" * 78)
    print("步骤 1：候选接入点连通性")
    print("-" * 78)
    alive = []
    for host, port, use_tls in ENDPOINTS:
        ok = probe_tcp(host, port)
        if ok is True:
            alive.append((host, port, use_tls))
            print("  ✅ %-32s :%-5d %-5s" % (host, port, "TLS" if use_tls else "明文"))
        else:
            print("  ❌ %-32s :%-5d %-5s  %s" % (host, port, "TLS" if use_tls else "明文", ok[1]))

    print()
    print("步骤 2：在可达接入点上扫 password 形态")
    print("-" * 78)

    # 生成 4 种 password
    et = T.TOKEN_EXPIRE_EPOCH
    res = "products/%s/devices/%s" % (pid, dev)
    sig_text = "%u\nsha256\n%s\n%s" % (et, res, T.TOKEN_VERSION)

    def token_with(key_bytes):
        mac = hmac.new(key_bytes, sig_text.encode("utf-8"), hashlib.sha256).digest()
        sign = "".join(T.url_encode_byte(ord(c))
                       for c in base64.b64encode(mac).decode("ascii"))
        return ("version=%s&res=products%%2F%s%%2Fdevices%%2F%s&et=%u&method=sha256&sign=%s"
                % (T.TOKEN_VERSION, pid, dev, et, sign))

    passwords = [
        ("密钥原文（44 字符 base64 串）", key_raw),
        ("解码后的 32 字符明文", key_dec.decode("ascii", "replace")),
        ("token（HMAC 密钥=解码后字节）", token_with(key_dec)),
        ("token（HMAC 密钥=原文串字节）", token_with(key_raw.encode("ascii"))),
    ]

    hits = []
    for host, port, use_tls in alive:
        for pname, pwd in passwords:
            code, note = connack(host, port, use_tls, ca_pem, dev, pid, pwd)
            if code is None:
                verdict = "（无 CONNACK）%s" % note
            else:
                verdict = "CONNACK=%d  %s" % (code, T.CONNACK_TEXT.get(code, "未知"))
                if code == 0:
                    hits.append((host, port, use_tls, pname))
            print("  %-32s :%-5d %-5s | %-30s %s"
                  % (host, port, "TLS" if use_tls else "明文", pname, verdict))

    print()
    print("=" * 78)
    if hits:
        print("🎉 找到可用的组合：")
        for h in hits:
            print("    %s:%d %s  password=%s" % (h[0], h[1], "TLS" if h[2] else "明文", h[3]))
    else:
        print("没有任何组合返回 CONNACK=0。")
        print("结论：接入点与鉴权形态也排除干净 → 只能是控制台里的三个值本身不一致。")
    print("=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
