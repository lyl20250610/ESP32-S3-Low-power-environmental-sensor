#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
一次性探针：确认 OneNET 的 token 里 `devices/{device_name}` 与 MQTT client_id
到底该填"控制台里显示的设备名称"还是"设备详情页 URL 里的那串数字 ID"。

背景：阶段 4 真机跑到 "DHT11 -> Wi-Fi -> TLS -> MQTT" 全通，服务端却回 CONNACK=4。
PC 端穷举过 44 个变体（算法/密钥编码/接入点/et/res 编码形态）全部返回 4，因为返回码
区分不了"签名错"和"资源不存在"，差分诊断走不通。后来用户在控制台发现：
设备名其实叫 `ESP32-S3`，而固件里写的是 `node01` —— 签名"对"了，但签的是
一个不存在的设备，所以永远 4。

这个脚本把"设备名写法"这一个维度单独拎出来打，顺便把旧名字当对照组。

用法：
    python tools/onenet_devname_probe.py
"""

import base64
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# 复用已经在用的实现，避免把 token 算法抄成第二份
import onenet_token_check as T  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECRETS = os.path.join(ROOT, "firmware", "main", "app_secrets.h")
ONENET_C = os.path.join(ROOT, "firmware", "components", "onenet", "onenet.c")

# 设备详情页 URL 里的数字 ID（/console/device/manage/2675462367/detail）
DEVICE_ID_IN_URL = "2675462367"

CANDIDATES = [
    ("ESP32-S3", "控制台里显示的设备名称"),
    (DEVICE_ID_IN_URL, "设备详情页 URL 里的数字 ID"),
    ("node01", "固件里原来写的（对照组，应当仍失败）"),
]

CONNACK_MEANING = {
    0: "连接成功",
    1: "协议版本不支持",
    2: "client_id 不合法",
    3: "服务端不可用",
    4: "用户名或密码错误（token / 凭据问题）",
    5: "未授权",
}


def read_macro(path, name):
    """从 app_secrets.h 抓 `#define NAME "value"`。"""
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(r'^#define\s+%s\s+"([^"]*)"' % re.escape(name), text, re.M)
    if not m:
        raise SystemExit("在 %s 里找不到 #define %s" % (path, name))
    return m.group(1)


def main():
    pid = read_macro(SECRETS, "ONENET_PRODUCT_ID")
    dev_key = read_macro(SECRETS, "ONENET_DEVICE_KEY")
    ca_pem = T.extract_ca_pem(ONENET_C)

    print("产品ID      : %s" % pid)
    print("设备密钥    : %s…（%d 字符，base64 解出 %d 字节）"
          % (dev_key[:12], len(dev_key), len(base64.b64decode(dev_key + "=" * (-len(dev_key) % 4)))))
    print("接入点      : %s:%d（TLS）" % (T.MQTT_HOST, T.MQTT_PORT))
    print()

    hit = None
    for idx, (name, note) in enumerate(CANDIDATES, 1):
        token, sig_text = T.generate_token(pid, name, dev_key)
        res = T.try_connect(ca_pem, username=pid, password=token, client_id=name)

        if res["connack"] is None:
            verdict = "❌ %s" % (res["error"] or "未知错误")
        elif res["connack"] == 0:
            verdict = "✅ CONNACK=0  连接成功"
            hit = name
        else:
            verdict = "❌ CONNACK=%d  %s" % (
                res["connack"], CONNACK_MEANING.get(res["connack"], ""))

        print("[%d] client_id = res 里的设备名 = %-14r" % (idx, name))
        print("    res 段  : %s" % sig_text.split("\n")[2])
        print("    说明    : %s" % note)
        print("    结果    : %s" % verdict)
        print()

        if hit:
            break

    print("=" * 68)
    if hit:
        print("结论：设备名必须写成 %r —— 把 firmware/main/app_secrets.h 的" % hit)
        print("      ONENET_DEVICE_NAME 改成它，重新编译烧录即可。")
        print("      （token 的 res 段与 MQTT client_id 用的是同一个值）")
    else:
        print("结论：三种写法都不通，说明问题不在设备名。")
        print("      下一步应检查产品ID / 设备密钥是否与控制台一致，")
        print("      或考虑该产品是否需要用「设备ID」而非设备名做 client_id。")
    return 0 if hit else 1


if __name__ == "__main__":
    sys.exit(main())
