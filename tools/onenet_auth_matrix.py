#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OneNET 鉴权**差分**诊断 —— 回答一个具体问题：

    "CONNACK=4 到底是在说『签名不对』，还是在说『这台设备/这个产品根本不存在』？"

为什么需要它
------------
`onenet_auth_probe.py` 已经把"签名算法"这一维穷举干净了（sha256/sha1/md5、
密钥 base64 解码与否、新旧接入点、et 取值），8 个变体一片 CONNACK=4。
继续穷举签名参数已经没有信息量了。

剩下唯一的怀疑对象是"产品ID / 设备名 / 设备密钥三个值本身与平台不一致"。
但**我们看不到 OneNET 内部**，所以换一个思路：喂给它几组**故意不存在**的
产品ID / 设备名，看返回码会不会变。

    · 如果"产品不存在"和"签名错"返回**同一个**码 → 服务端是黑盒，
      此路不通，只能人工去控制台核对三元组。
    · 如果"产品不存在"返回**另一个**码 → 我们就获得了一个判据：
      用它可以反推出现在这三个值里**到底哪一个**是错的。

这比坐在那里猜有用得多，成本只有几十秒。

用法
----
    python tools/onenet_auth_matrix.py

只做只读探测：建 TLS、发 CONNECT、读 CONNACK、断开。不发布任何数据。
"""

import argparse
import base64
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import onenet_auth_probe as A   # noqa: E402
import onenet_token_check as T  # noqa: E402


# 一个**必然不存在**的产品ID。长度/字符集都模仿真实值，避免被"格式不合法"提前拒掉。
BOGUS_PID = "Zz9Qq7Ww1X"
# 一个**必然不存在**的设备名。
BOGUS_DEV = "definitely_not_a_device"

# 一段固定字节的假密钥（32 字节，与真实密钥解码后等长）。
FAKE_KEY = bytes(range(32))


def show(idx, label, code, note):
    if code is None:
        verdict = "（无 CONNACK）%s" % note
    else:
        verdict = "CONNACK=%d  %s" % (code, T.CONNACK_TEXT.get(code, "未知返回码"))
    print("  [%d] %-46s %s" % (idx, label, verdict))


def main():
    ap = argparse.ArgumentParser(description="OneNET 鉴权差分诊断")
    ap.add_argument("--secrets", default=A.DEFAULT_SECRETS)
    ap.add_argument("--onenet-c", default=A.DEFAULT_ONENET_C)
    args = ap.parse_args()

    secrets = T.load_secrets(args.secrets)
    pid = secrets["ONENET_PRODUCT_ID"]
    dev = secrets["ONENET_DEVICE_NAME"]
    key = secrets["ONENET_DEVICE_KEY"]
    ca_pem = T.extract_ca_pem(args.onenet_c)

    print("=" * 78)
    print("配置来源：%s" % args.secrets)
    print("  产品ID   = %r" % pid)
    print("  设备名   = %r" % dev)
    print("  设备密钥 = %d 字符（不打印内容）" % len(key))
    print("=" * 78)

    host, port = A.HOSTS["studio"]
    et = T.TOKEN_EXPIRE_EPOCH

    # 真实 token 与"密钥故意错"的 token
    real_token = A.build_token(pid, dev, base64.b64decode(key + "=" * (-len(key) % 4)),
                               "sha256", et)
    fake_token_real_res = A.build_token(pid, dev, FAKE_KEY, "sha256", et)
    fake_token_bogus_dev = A.build_token(pid, BOGUS_DEV, FAKE_KEY, "sha256", et)
    fake_token_bogus_pid = A.build_token(BOGUS_PID, dev, FAKE_KEY, "sha256", et)

    print()
    print("第一组：把『设备/产品是否存在』这一维单独拉出来看")
    print("-" * 78)
    cases = [
        ("真产品 + 真设备 + 真密钥     （已知=4，作为基线）",
         pid, real_token, dev),
        ("真产品 + 真设备 + 假密钥",
         pid, fake_token_real_res, dev),
        ("真产品 + 【不存在的设备】 + 假密钥",
         pid, fake_token_bogus_dev, BOGUS_DEV),
        ("【不存在的产品】 + 真设备 + 假密钥",
         BOGUS_PID, fake_token_bogus_pid, dev),
        ("【不存在的产品】 + 【不存在的设备】 + 乱码密码",
         BOGUS_PID, "not-a-token-at-all", BOGUS_DEV),
    ]
    for i, (label, user, pwd, cid) in enumerate(cases, 1):
        code, note = A.attempt(ca_pem, host, port, user, pwd, cid)
        show(i, label, code, note)

    print()
    print("第二组：username / client_id 的角色会不会和我们以为的相反")
    print("-" * 78)
    cases2 = [
        ("username=设备名, client_id=产品ID（对调）",
         dev, real_token, pid),
        ("username=空, client_id=设备名",
         "", real_token, dev),
        ("username=产品ID, client_id=产品ID（client_id 拆开）",
         pid, real_token, pid),
        ("username=产品ID, client_id=另一个名字",
         pid, real_token, "another-client"),
    ]
    for i, (label, user, pwd, cid) in enumerate(cases2, len(cases) + 1):
        code, note = A.attempt(ca_pem, host, port, user, pwd, cid)
        show(i, label, code, note)

    print()
    print("=" * 78)
    print("判读方法：")
    print("  · 第 2/3/4/5 行若**都等于**第 1 行 → 服务端不区分『设备不存在』与『签名错』，")
    print("    差分辨不出东西，必须去控制台人工核对三元组。")
    print("  · 若第 3 行或第 4 行出现**不同的**返回码 → 该码就是『值填错了』的指纹，")
    print("    据此可定位到是产品ID还是设备名填错。")
    print("=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
