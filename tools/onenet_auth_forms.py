#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OneNET token —— 最后一批"token 形态"假设排查。

背景
----
板子实测（阶段 4，2026-10-11）已经证明：

    Wi-Fi 连上 602 → 192.168.1.102 → TLS 握手成功（tls_last_err=0x0）
    → MQTT 到达 studio-mqtts.heclouds.com:8883 → 服务端回 CONNACK=4

也就是说**链路全通，只剩鉴权**。而 `onenet_auth_matrix.py` 已证明服务端不区分
"资源不存在"与"签名错"（9 组变体全是 4），所以换一条思路：
把 token **本身的形态**再扫一遍。

还没试过的形态
--------------
1. `et` 给"很近的将来"。我们一直用 2524608000（≈2050）。万一新版平台对
   et 有上限（"不许超过 N 天"），远期值反而会被判过期/非法。
2. token 里 `res` 和 `sign` 的 **URL 编码**。文档要求编码 `/` 和 `=`，
   但那是从 HTTP query string 继承来的规矩；MQTT 传的是原始字节，
   服务端有可能期望**未编码**的形态。AloT 用编码形态，但 AloT 这条链路
   本来就从来没通过（它连 CN 校验都过不去），不能作为证据。

这两条都是"一改就中"或"彻底排除"的二值假设，成本几十秒。

用法
----
    python tools/onenet_auth_forms.py
"""

import argparse
import base64
import hashlib
import hmac
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import onenet_auth_probe as A   # noqa: E402
import onenet_token_check as T  # noqa: E402


def mk_token(pid, dev, key_bytes, et, method="sha256", enc_res=True, enc_sign=True):
    """自由控制 res / sign 是否 URL 编码的 token 生成器。"""
    res = "products/%s/devices/%s" % (pid, dev)
    sig_text = "%u\n%s\n%s\n%s" % (et, method, res, T.TOKEN_VERSION)
    mac = hmac.new(key_bytes, sig_text.encode("utf-8"), hashlib.sha256).digest()
    sign_b64 = base64.b64encode(mac).decode("ascii")

    sign = "".join(T.url_encode_byte(ord(c)) for c in sign_b64) if enc_sign else sign_b64
    res_out = res.replace("/", "%2F") if enc_res else res

    return "version=%s&res=%s&et=%u&method=%s&sign=%s" % (
        T.TOKEN_VERSION, res_out, et, method, sign)


def main():
    ap = argparse.ArgumentParser(description="OneNET token 形态排查")
    ap.add_argument("--secrets", default=A.DEFAULT_SECRETS)
    ap.add_argument("--onenet-c", default=A.DEFAULT_ONENET_C)
    args = ap.parse_args()

    secrets = T.load_secrets(args.secrets)
    pid = secrets["ONENET_PRODUCT_ID"]
    dev = secrets["ONENET_DEVICE_NAME"]
    key_b64 = secrets["ONENET_DEVICE_KEY"].strip()
    key_b64 += "=" * (-len(key_b64) % 4)
    key = base64.b64decode(key_b64)
    ca_pem = T.extract_ca_pem(args.onenet_c)

    host, port = A.HOSTS["studio"]
    now = int(time.time())

    print("=" * 78)
    print("PC 当前时间 = %d（%s）" % (now, time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now))))
    print("=" * 78)

    scenarios = [
        # (说明, token)
        ("et=2050（基线，已知=4）",
         mk_token(pid, dev, key, T.TOKEN_EXPIRE_EPOCH)),
        ("et=now+3600（1 小时后）",
         mk_token(pid, dev, key, now + 3600)),
        ("et=now+86400（1 天后）",
         mk_token(pid, dev, key, now + 86400)),
        ("et=now+2592000（30 天后）",
         mk_token(pid, dev, key, now + 2592000)),
        ("et=now+31536000（1 年后）",
         mk_token(pid, dev, key, now + 31536000)),
        ("et=now+3600 且 res 不编码（保留 /）",
         mk_token(pid, dev, key, now + 3600, enc_res=False)),
        ("et=now+3600 且 sign 不编码（保留 + 与 =）",
         mk_token(pid, dev, key, now + 3600, enc_sign=False)),
        ("et=now+3600 且 res 与 sign 都不编码",
         mk_token(pid, dev, key, now + 3600, enc_res=False, enc_sign=False)),
        ("et=2050 且 res 与 sign 都不编码",
         mk_token(pid, dev, key, T.TOKEN_EXPIRE_EPOCH, enc_res=False, enc_sign=False)),
        ("et=now+3600，HMAC 密钥用未解码的 44 字符密钥",
         mk_token(pid, dev, key_b64.encode("ascii"), now + 3600)),
        ("et=now+3600，method=sha1",
         mk_token(pid, dev, key, now + 3600, method="sha1")),
    ]

    # 逐条改写：mk_token 里写死了 sha256 的 digestmod，method 只影响签名串。
    # 为了让 sha1 那条真的用 sha1，这里单独重算一次。
    import hashlib as _hl
    res = "products/%s/devices/%s" % (pid, dev)
    sig_text = "%d\nsha1\n%s\n%s" % (now + 3600, res, T.TOKEN_VERSION)
    mac = hmac.new(key, sig_text.encode("utf-8"), _hl.sha1).digest()
    sign = "".join(T.url_encode_byte(ord(c)) for c in base64.b64encode(mac).decode("ascii"))
    scenarios[-1] = ("et=now+3600，method=sha1",
                     "version=%s&res=%s&et=%d&method=sha1&sign=%s"
                     % (T.TOKEN_VERSION, res.replace("/", "%2F"), now + 3600, sign))

    for i, (label, token) in enumerate(scenarios, 1):
        code, note = A.attempt(ca_pem, host, port, pid, token, dev)
        verdict = ("CONNACK=%d  %s" % (code, T.CONNACK_TEXT.get(code, "未知"))
                   if code is not None else "（无 CONNACK）%s" % note)
        print("  [%2d] %-46s %s" % (i, label, verdict))

    print()
    print("=" * 78)
    print("任何一条出现 CONNACK=0 → 就是它，问题出在 token 形态而不是凭据。")
    print("全部仍是 4 → token 形态也排除干净，只能人工核对控制台三元组。")
    print("=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
