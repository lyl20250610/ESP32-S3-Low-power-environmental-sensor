# OneNET 控制台配置清单（步骤 0 / 步骤 4）

> 平台：中国移动 OneNET —— <https://open.iot.10086.cn>
> 目标：建出产品 + 设备 `node01` + **9 个物模型属性**，并把三元组填进 `firmware/main/app_secrets.h`
> 协议的来源：本工程复用参考工程 AloT 已跑通的 OneNET 配置
> （`studio-mqtts.heclouds.com:8883`，TLS，MQTT 3.1.1，主题 `$sys/{产品ID}/{设备名}/thing/property/post`）

---

## 0. 先决条件

| # | 事项 | 说明 |
|---|---|---|
| 1 | 账号**实名认证** | 未实名无法创建产品和设备。控制台右上角头像 → 实名认证 |
| 2 | 记录**产品ID（pid）** | **OneNET Studio（新版）是 10 位字母+数字短串**（本项目实际值形如 `EyF5sX8P09`）；旧版"多协议接入"才是纯数字。抄的时候**复制粘贴，不要手打**——`0/O`、`l/I/1` 打错一个字符，现象是永远 `CONNACK=4` |
| 3 | ⚠️ **不要复用 AloT 的产品/设备** | 两台设备用同一套 `client_id` 登录会互相顶下线，现象是"连上 → 秒断 → 重连"，极难排查 |

---

## 1. 创建产品

控制台 → **产品开发 → 创建产品**

| 字段 | 填什么 |
|---|---|
| 产品名称 | `ESP32-S3环境监测节点`（随意，中文可） |
| 所属行业 / 类别 | 智能家居 / 环境监测（随意，不影响数据） |
| **智能类型** | 选 **自定义**（不要选"设备接入"以外的模式） |
| **数据格式** | ★ 必须选 **物模型（OneJSON）**，不要选"透传/自定义" |
| 联网方式 | **Wi-Fi** |
| 协议 | **MQTT** |
| 认证方式 | **设备密钥**（Key 鉴权） |
| 描述 | 一款基于ESP32的远程低功耗环境检测器 |

建完后在**产品详情页**记下 **产品ID**（形如一串 6~8 位数字），后面 `ONENET_PRODUCT_ID` 用它。

---

## 2. 创建设备

产品 → **设备管理 → 添加设备**

| 字段 | 填什么 |
|---|---|
| 设备名称 | **`node01`** ← 必须一字不差，固件里 `ONENET_DEVICE_NAME` 是它 |
| 设备密钥 | 选"由系统自动生成"，保存后**点"显示"抄下来** |
| 备注 | 楼道/室内节点 |

抄下**设备密钥** → `ONENET_DEVICE_KEY`。

---

## 3. ★ 创建 9 个物模型属性（本文件的核心）

进入**产品 → 物模型管理（功能定义）→ 属性 → 添加属性**，逐个建下面 9 个。

> ⚠️ **标识符（Identifier）必须一字不差**，全部小写英文 + 下划线。
> 平台上没定义的标识符，上报会被丢弃或报错，现象就是"串口显示发出去了，平台却没有数据"。

### 3.1 对照总表

| # | 标识符 | 功能名称 | 数据类型 | 取值范围 | 步长 | 单位 | 读写类型 | 上报频率 |
|---|---|---|---|---|---|---|---|---|
| 1 | `temperature` | 温度 | float（单精度浮点） | -10 ~ 60 | 0.1 | ℃ | **只读** | 每次唤醒（10 分钟） |
| 2 | `humidity` | 湿度 | float（单精度浮点） | 5 ~ 95 | 0.1 | %RH | **只读** | 每次唤醒（10 分钟） |
| 3 | `rssi` | Wi-Fi信号强度 | int32 | -120 ~ 0 | 1 | dBm | **只读** | 每次唤醒 |
| 4 | `seq` | 上报序号 | int32 | 0 ~ 2000000000 | 1 | （留空） | **只读** | 每次唤醒 |
| 5 | `sensor_ok` | 温湿度有效 | bool | 真值=读取成功，假值=读取失败 | — | — | **只读** | 每次唤醒 |
| 6 | `wake_reason` | 唤醒原因 | int32 | 0 ~ 15 | 1 | （留空） | **只读** | 每次唤醒 |
| 7 | `publish_failures` | 累计上报失败次数 | int32 | 0 ~ 100000 | 1 | （留空） | **只读** | 每次唤醒 |
| 8 | `co2` | CO2等效浓度 | int32 | 400 ~ 60000 | 1 | ppm | **只读** | **每 30 分钟** |
| 9 | `tvoc` | TVOC | int32 | 0 ~ 60000 | 1 | ppb | **只读** | **每 30 分钟** |

**全部 9 个的「读写类型」都选"只读"** —— 这些值只由设备上报，平台不需要下发。
（本项目不使用平台的"属性设置"下发功能，因此节点代码里不订阅 `.../property/set`。）

### 3.2 每个属性的"描述"建议这么填

- `temperature` / `humidity` → `DHT11 读取，失败时不上报（sensor_ok=false）`
- `rssi` → `MQTT 连接时的 Wi-Fi 信号强度，用于评估现场覆盖`
- `seq` → `节点侧单调递增的序号，服务端据此检查丢包`
- `sensor_ok` → `DHT11 两次读取都失败时为 false，绝不用上次的值伪装`
- `wake_reason` → `ESP32 唤醒原因：0=冷启动，4=定时器`
- `publish_failures` → `RTC 内存里累计的上报失败次数，深睡不清零`
- `co2` → `SGP30 等效二氧化碳，每 30 分钟采样一次`
- `tvoc` → `SGP30 总挥发性有机物，每 30 分钟采样一次`

### 3.3 单位下拉框里没有 `dBm` / `%RH` / `ppb` 怎么办

**留空即可，不影响任何数据。** 真正决定能否上报成功的是**标识符**，
单位只是给平台页面显示用的。`℃`、`ppm` 通常能在下拉框里找到。

### 3.4 建完自检

回到 **设备 → node01 → 设备数据流（或"物模型数据"）**，
此时应该能看到 9 个属性名，值都是空的（还没有设备上报过）。
**看到 9 个名字 = 这一步做对了。**

---

## 4. 把三元组填进固件

```powershell
cd D:\esp32\xiangmu1
Copy-Item firmware\main\app_secrets.h.example firmware\main\app_secrets.h
notepad firmware\main\app_secrets.h
```

填这 4 个（外加 Wi-Fi）：

```c
#define WIFI_SSID           "你的2.4GHz WiFi名"
#define WIFI_PASSWORD       "你的WiFi密码"
#define ONENET_PRODUCT_ID   "第1步记下的产品ID"
#define ONENET_DEVICE_NAME  "node01"
#define ONENET_DEVICE_KEY   "第2步抄下的设备密钥"
```

> 🔒 `app_secrets.h` 已被 `.gitignore` 排除，本仓库是 **PUBLIC** 仓库，**绝不能提交它**。
> 填完请跑一次 `git status`，确认输出里没有 `app_secrets.h`。
>
> 注意：ESP32-S3 只支持 **2.4GHz** Wi-Fi，5GHz 的 SSID 连不上。

---

## 5. 创建"应用"以便 PC 端拉取历史数据（步骤 9 才需要）

24h 验收需要一个 PC 脚本定时把数据拉下来。OneNET 的做法是创建**应用**并生成 **access_key**：

控制台 → **应用管理 → 创建应用** → 创建后能看到 **access_key** 与 **user_id**。

把这两个值填到 `app_secrets.h` 里的 `ONENET_API_ACCESS_KEY` / `ONENET_API_USER_ID`。

- 接口文档：<https://open.iot.10086.cn/doc/iot_platform/book/api/application/queryDevicePropertyHistory.html>
- 备选路径（若应用 API 受限）：控制台 **规则引擎 → 消息队列MQ / HTTP推送**，把设备数据推给你的服务

---

## 6. 常见坑速查

| 现象 | 原因 |
|---|---|
| 串口显示 MQTT connected，但平台没有数据 | 9 个属性没建全，或标识符拼错（最常见） |
| 连上立刻断开、反复重连 | 同一产品/设备被另一台设备（如 AloT）用同样的 `client_id` 登录，互相顶下线 |
| MQTT 直接连不上 | 用了 5GHz Wi-Fi；或产品 ID/设备密钥抄错；或账号未实名 |
| 数据能进平台但历史查不到 | 免费版数据存储时长有限；或没建应用/没开通数据存储 |
| 平台报"设备不存在" | 设备名不是 `node01`，或建在了别的产品下 |

### 6.1 两个"看起来像坑、其实不是坑"的现象

这两个都在 2026-10-11 实测遇到过，各花了不少时间，记下来免得重蹈。

#### （1）OneNET 的服务端证书自签，且 CN 不是主机名 → 必须跳过 CN 校验

实测 `studio-mqtts.heclouds.com:8883` 返回的证书：

| 项 | 值 |
|---|---|
| subject / issuer | 都是 `C=CN, O=CMIOT, CN=OneNET MQTTS`（**自签名**） |
| 有效期 | 2019-05-29 → 2049-05-21 |
| subjectAltName | **完全没有这个扩展** |
| DER 大小 | 831 字节 |

mbedTLS 在证书**没有 SAN 扩展时会回退去比 CN**（`x509_crt.c` 的 `x509_crt_verify_name()`），
而 CN 是 `OneNET MQTTS`、主机名是 `studio-mqtts.heclouds.com` → 不匹配 →
置 `MBEDTLS_X509_BADCERT_CN_MISMATCH` → `esp_tls_mbedtls.c` 只要 `verify_result != 0`
就判握手失败 → **永远走不到 MQTT**。

所以 `firmware/components/onenet/onenet.c` 里 **`skip_cert_common_name_check = true` 是必须的**。
注意这**不是**"关掉证书校验"：CA 仍然钉死在内嵌的 PEM 上、证书链仍然严格验证，
被跳过的只是"证书里的名字是否等于主机名"这一条——对一个自签、且 CN 本来就不是主机名的
证书而言，这条检查提供不了任何额外的身份保证。

> 参考工程 AloT 的 `components/MY_MQTT/mqtt.c:328` 写的是 `false` 且不设 `common_name`，
> **它这条链路是走不通的**，不要照抄。

顺带一个工具层面的坑：**Python 从 3.7 起彻底取消了"没有 SAN 时回退比 CN"**
（OpenSSL 的 `X509_CHECK_FLAG_NEVER_CHECK_SUBJECT`），所以用 Python 写探针脚本时，
即使固件这条路是通的，Python 也会报 `Hostname mismatch`。
`tools/onenet_token_check.py` 因此改成 `check_hostname=False` + `CERT_REQUIRED`
+ 手动按 mbedTLS 语义比 SAN/CN，这样它和固件的行为才一致。

#### （2）`CONNACK = 4` 并不等于"设备不存在"

在我们这类失败下，服务端**一律**回 **CONNACK=4（用户名或密码错误）**——
连"产品ID 和设备名都故意填成不存在的值"也一样。我们用 `tools/onenet_auth_matrix.py`
喂了 9 组变体（不存在的产品、不存在的设备、乱码密码、username/client_id 对调、
空 username…），**9 组全部返回 4**。也就是说平台在这件事上是黑盒，
返回码**无法**区分"签名错"与"资源不存在"。

因此排查顺序应当是：

1. `tools/onenet_token_check.py` → 验证 **token 算法 + 凭据能否建 TLS**（不需要板子）
2. `tools/onenet_auth_matrix.py` → 看返回码有没有区分度（当前结论：没有，此路不通）
3. 剩下的**只能人工核对控制台三元组**：产品ID、设备名称、设备密钥
   （设备详情里点「显示」后**完整复制，含结尾的 `=`**）

已经排除掉的可能：签名算法（sha256 / sha1 / md5）、设备密钥是否要 base64 解码后再做 HMAC、
新旧接入点（`studio-mqtts.heclouds.com` 与 `mqtts.heclouds.com`）、`et` 的取值、
token 中 URL 编码的字符集（与官方文档逐字一致）。

---

## 7. 本步骤完成标志

- [ ] 产品已创建，**数据格式 = 物模型**
- [ ] 设备 `node01` 已添加
- [ ] **9 个属性全部建好**（回到设备数据流页能看到 9 个名字）
- [ ] 三元组已填入 `app_secrets.h`
- [ ] `git status` 输出里**没有** `app_secrets.h`
