# ESP32-S3 超低功耗无线环境监测节点 — 实施计划

> 依据：`ESP32-S3超低功耗无线环境监测节点项目搭建方案.docx`（文档版本 **1.1**）
> 制定：2026-10-08 · 修订：2026-10-08 **v2**（三项范围变更）
> 工作区：`D:\esp32\xiangmu1`

## 变更记录

| 版本 | 变更 | 触发 |
|---|---|---|
| v1 | 初版：本机 Mosquitto + 仅 DHT11 | — |
| **v2** | ① MQTT 平台改为 **中国移动 OneNET**（弃用本机 Mosquitto）；② 新增 **SGP30** CO₂/TVOC 传感器（每 30 分钟采一次）；③ 板子确认为 **ESP32-S3-DevKitC-1 v1.1 + WROOM-1-N8R8**；④ 取消 CO（一氧化碳）检测 | 用户 m00164 决策 |

> ⚠️ **v2 推翻 v1 的一条结论**：v1 判定 AloT 的 `MY_MQTT` 组件"是 OneNET 专有、不能复用，必须新写 `mqtt_pub`"。改用 OneNET 后该结论反转——`MY_MQTT` 正是本项目要复用的组件，改造量从"新写"降为"改 2 处 + 加超时"。

---

## 0. 已确认的环境事实（2026-10-08 实测）

| 项 | 实测结果 | 证据 |
|---|---|---|
| ESP-IDF | **v5.4.3**，`D:\esp\v5.4.3\esp-idf` | `git describe --tags`；`esp_idf_version.h` MAJOR=5 MINOR=4 PATCH=3 |
| esp-mqtt | IDF 自带，`D:\esp\v5.4.3\esp-idf\components\mqtt` | 目录存在 |
| 交叉工具链 | `C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20250730` | 目录存在 |
| IDF Python 环境 | `C:\Espressif\python_env\idf5.4_py3.11_env` | 目录存在 |
| 串口 | COM3、COM4 | `[System.IO.Ports.SerialPort]::GetPortNames()` |
| VS Code | `D:\VS\Microsoft VS Code\bin\code.cmd`，已装 `espressif.esp-idf-extension-2.3.0` | 扩展目录列表 |
| Arduino IDE / arduino-cli / PlatformIO | **均未安装** | `Get-Command` 全部为空 |
| 可复用参考工程 | `D:\esp32\ESP32-S31\ESP32-S3\AloT`（target=esp32s3，8MB flash，Octal PSRAM） | `sdkconfig.defaults` |
| git | `C:\Users\Lenovo\scoop\apps\git\current\bin\git.exe` | `Get-Command git` |
| 网络（GitHub） | **不稳定**：`git ls-remote` 首次成功、二次报 `curl 28 Recv failure: Connection was reset` | 两次 `git ls-remote` 结果不一致 |
| 网络（OneNET 文档） | 可达（HTTP 200） | `open.iot.10086.cn` 抓取成功 |

### 0.1 用户已确认的硬件与平台决策

| 项 | 结论 | 来源 |
|---|---|---|
| 开发板 | **ESP32-S3-DevKitC-1，硬件版本 v1.1** | 用户 m00134 |
| 模组 | **ESP32-S3-WROOM-1-N8R8**（8MB Flash + 8MB Octal PSRAM） | 用户 m00164 |
| 温湿度传感器 | **DHT11，3 针带小板模块**（自带 4.7k~10k 上拉，不需外接） | 用户 m00100 |
| 气体传感器 | **SGP30**（eCO₂ + TVOC，I2C），**每 30 分钟采样一次** | 用户 m00208 |
| CO（一氧化碳） | **不做** | 用户 m00208 |
| MQTT 平台 | **OneNET Studio**，`studio-mqtts.heclouds.com:8883`，**TLS** | 用户 m00164 / m00208 |
| OneNET 设备 | **新建独立产品 + 新设备 `node01`**（不与 AloT 共用，避免 client_id 互踢） | 用户 m00208 |
| 框架 | **ESP-IDF 5.4.3** | 用户 m00096 |
| 远端仓库 | `https://github.com/lyl20250610/ESP32-S3-Low-power-environmental-sensor.git`（public，默认分支 `main`，仅 1 个 93 字节 README，提交 `4bac871`） | GitHub API |
| 接线状态 | **尚未接线** | 用户 m00096 |

### 0.2 本机 git 凭据（首次 push 前必须解决）

全局 `user.name` / `user.email` **未配置**；`credential.helper` **未配置**；Windows 凭据管理器无 GitHub 凭据；`gh` CLI 不存在。可用凭据助手存在于 `C:\Users\Lenovo\scoop\apps\git\current\mingw64\bin\git-credential-manager.exe`（未在 PATH）。

### 0.3 本机 ESP-IDF 构建环境（步骤 1 实测，5 个坑已绕过）

**结论：不要直接用 ESP-IDF 的 `export.ps1`，一律用本工程自带的 `tools\idf.ps1`（或 `tools\idf.cmd`）。**

| # | 症状 | 原因 | 绕过方式 |
|---|---|---|---|
| 1 | `idf.py` 不是可识别的 cmdlet | PATH 上的 `python` 是 Microsoft Store 占位程序，`activate.py` 跑不了 → `export.ps1:27` 抛 `BadExpression` | 把 `C:\Espressif\python_env\idf5.4_py3.11_env\Scripts` 放到 PATH 最前 |
| 2 | `找不到 ESP-IDF：build\tools\idf.py 不存在`；`-p` 报未知参数 | 包装脚本声明了 `param()`，被 PowerShell 抢走 `build` / `-p` | 包装脚本不声明 `param()`，用 `$args` 转发 |
| 3 | 中文注释变 `鎵句笉鍒?IDF…` / `ParseError` | PowerShell **5.1** 按 GBK 解码**无 BOM** 的 UTF-8 `.ps1` | `.ps1` 存成 **UTF-8 with BOM** |
| 4 | `NativeCommandError` @ `export.ps1:24`，之后静默、退出码 1 | `activate.py` 的 stderr 在 `$ErrorActionPreference='Stop'` 下被当成 terminating ErrorRecord | 点源时临时 `'Continue'` + `*> $null` |
| 5 | `无法加载文件 …未对文件进行数字签名…` | `D:\esp32\xiangmu1\` **及其子目录**下的 `.ps1` 被按路径拦截（`$env:TEMP`/`D:\esp`/`D:\esp32` 下同名文件正常）→ 路径相关、非内容相关 | `-ExecutionPolicy Bypass`，或用 `tools\idf.cmd` |

已验证的工具路径：`idf.py` = `C:\Espressif\tools\idf-exe\1.0.3\idf.py.exe`；`xtensa-esp32s3-elf-gcc` = `C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20250730\bin`；`cmake` = `C:\Espressif\tools\cmake\3.30.2\bin\cmake.exe`；`ninja` = `C:\Espressif\tools\ninja\1.12.1\ninja.exe`。

**坑 6（步骤 2 撞上，属于构建系统而非环境）**：`idf_component_register()` 的 `REQUIRES` 一旦写了内容，组件就**不再**享有"没写 REQUIRES 时自动依赖全部组件"的旧行为，只能看见自己声明的依赖 + ESP-IDF 公共依赖（esp_common / freertos / log / heap / esp_system / newlib / soc / hal…）。

- 症状：`D:/esp32/xiangmu1/firmware/main/main.c:27:10: fatal error: esp_flash.h: No such file or directory`，后面附一句 `Compilation failed because main.c (in "main" component) includes esp_flash.h, provided by spi_flash component(s). … add spi_flash to PRIV_REQUIRES list of idf_component_register call in D:\esp32\xiangmu1\firmware\main\CMakeLists.txt.`
- 原因：步骤 1 的 `main/CMakeLists.txt` **没有** `REQUIRES`，所以那时 `esp_flash.h` / `esp_chip_info.h` / `esp_app_desc.h` 全都白拿；步骤 2 为了用 `dht11` 加上了 `REQUIRES dht11`，这些"白拿"的组件同时被切断。
- 修法：把用到的头文件提供方逐个显式列出。`main/CMakeLists.txt` 现为
  `REQUIRES dht11` + `PRIV_REQUIRES esp_hw_support esp_app_format spi_flash`
  （分别对应 `esp_chip_info.h`/`esp_sleep.h`、`esp_app_desc.h`、`esp_flash.h`）。
- 教训：**报错信息最后那句 "add X to PRIV_REQUIRES" 就是答案，照做即可**；遇到 `fatal error: xxx.h: No such file or directory` 且路径确实存在于 IDF 里时，先怀疑 REQUIRES，而不是怀疑 IDF 装坏了。

**坑 7（步骤 2 收尾时撞上，属于本机网络环境）**：`git fetch` / `git push` 连不上 GitHub，而浏览器和 PowerShell 的 `Invoke-WebRequest https://github.com` 却返回 200。

- 症状：`fatal: unable to access 'https://github.com/…': Failed to connect to github.com port 443 after 21093 ms: Could not connect to server`（偶尔是 `Recv failure: Connection was reset`）；`curl.exe` 同样超时（`curl: (28)`）。
- 根因：本机系统代理（**MyClash**）在 `127.0.0.1:7877`。浏览器 / VS Code / `Invoke-WebRequest` 会自动读 WinINET 代理设置，而 **`git.exe` 与 `curl.exe` 不会**，于是它们直连 `github.com`（解析为 `20.205.243.166`）超时。
- 诊断（"网页能开、命令行连不上"时先跑这两条）：
  ```powershell
  Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' |
      Select-Object ProxyEnable, ProxyServer
  Get-Process | Where-Object ProcessName -match 'clash|v2ray|xray|sing-box'
  ```
- 修法（已对本仓库配好，只影响本仓库）：
  ```powershell
  git config http.proxy  http://127.0.0.1:7877
  git config https.proxy http://127.0.0.1:7877
  ```
  代理关掉时 git 会连不上，记得 `git config --unset http.proxy` / `--unset https.proxy`。
- 顺带查明的凭据现状：全局 `credential.helper` 是 Git Credential Manager
  （`C:\Users\Lenovo\scoop\apps\git\2.50.1\mingw64\bin\git-credential-manager.exe`），
  **GitHub 凭据已缓存**，因此 `git push` 无需再输密码。

---

## 1. 目标与范围

**总目标**：按项目书完成节点，跑通「采样 → Wi-Fi → OneNET → 深度睡眠」闭环。

- **本计划主体 = 项目书第一阶段**（功能闭环），外加新增的 SGP30 气体检测。
- **第 9 步 = 项目书第二阶段**（电池供电与功耗优化）。
- **不在本计划内**：原第三阶段的光照/气压（BH1750/BME280）、第四阶段 ESP-NOW 多节点。

### 1.1 相对项目书的**有意偏离**（逐条记录理由）

| # | 项目书原文 | 本计划 | 理由 |
|---|---|---|---|
| D1 | 5.1 节：主题 `env/node01/telemetry`，自建 broker | OneNET 物模型主题 `$sys/{pid}/{dev}/thing/property/post` | 用户 m00164 改用 OneNET；OneNET 强制该主题结构 |
| D2 | 5.2 节：扁平 JSON `{"node":...,"temperature_c":...}` | OneJSON 嵌套 `{"id":...,"version":"1.0","params":{"temperature":{"value":24.6}}}` | OneNET 物模型协议要求；扁平 JSON 会被平台丢弃 |
| D3 | 5.3 节：Mosquitto + MQTT Explorer + Node-RED | OneNET 控制台 + 本地拉取脚本 | 同上；且省掉本机装服务端 |
| D4 | 6.3 节示例：QoS 0 | **QoS 1**（`mqtt.c:284` 原代码已是 QoS1） | 项目书 1.3 节要求"服务端按序号检查缺失"，QoS0 无法验证是否送达 |
| D5 | 无 CO₂ 要求 | 新增 SGP30，CO₂ 每 30 分钟 | 用户 m00164 要求增加 CO₂ 检测 |
| D6 | 无 | 关闭 PSRAM（`CONFIG_SPIRAM=n`） | 本应用不需要 8MB PSRAM；Octal PSRAM 占 GPIO33~37 且增加功耗。**注意 AloT 的 `sdkconfig.defaults` 是开着的，照抄会误开** |

> 停止条件第 5 条要求"需要改动项目书既定指标时停下确认"。以上 6 条偏离均已由用户明确决策或属平台强制，**不构成新的未决项**。

**物理边界（我不能替代你做的部分）**：接线、插拔 USB、万用表/功耗仪测量、电池与稳压电路焊接、OneNET 控制台网页上的注册/实名/建产品操作。
**我能做**：全部固件代码、编译、服务端与拉取脚本、统计脚本、执行烧录命令、分析串口日志与数据。

---

## 2. 架构设计

### 2.1 工程布局（新建独立工程，不污染 AloT）

```
D:\esp32\xiangmu1\
├─ README.md                      项目总览 + 进度看板
├─ .gitignore                     ★ 第一件事：排除 **/app_secrets.h
├─ docs\PLAN.md                   本文件
├─ docs\WALKTHROUGH.md            逐步讲解
├─ docs\HARDWARE.md               接线确认单（步骤 0 产出）
├─ firmware\                      ESP-IDF 工程
│  ├─ CMakeLists.txt
│  ├─ sdkconfig.defaults          ★ 8MB flash、SPIRAM 关闭
│  ├─ partitions.csv
│  ├─ main\
│  │  ├─ CMakeLists.txt
│  │  ├─ main.c                   一次唤醒周期的业务编排
│  │  ├─ app_config.h             引脚/周期/超时集中配置
│  │  ├─ app_secrets.h(.example)  ★ Wi-Fi + OneNET 凭据（真实文件不入库）
│  │  └─ app_payload.c/.h         OneJSON 组装（纯函数，可单测）
│  └─ components\
│     ├─ dht11\                   ★ 移植自 AloT components\DHT11，改造
│     ├─ wifi_sta\                ★ 移植自 AloT components\WIFI，改造
│     ├─ onenet\                  ★ 移植自 AloT components\MY_MQTT，改造
│     └─ sgp30\                   ★ 移植自 AloT components\SGP30，改造
└─ tools\
   ├─ dump_docx.py                项目书导出（已用）
   ├─ onenet_pull.py              ★ 定时拉取 OneNET 历史数据 → CSV
   └─ analyze_run.py              24h 验收统计
```

### 2.2 数据链路

```
ESP32-S3 ──Wi-Fi──> OneNET Studio ──应用API(设备属性记录查询)──> PC 拉取脚本 ──> CSV ──> 验收统计
                         │
                         └── 控制台物模型页面 / 数据流（人工查看）
```

**没有本机 broker 了**（用户 m00164 决策）。24h 验收改为：PC 用 OneNET **应用 API「设备属性记录查询」**定时拉历史数据落库。
- 文档：<https://open.iot.10086.cn/doc/iot_platform/book/api/application/queryDevicePropertyHistory.html>（实测 HTTP 200，属"应用开发类"API）
- 鉴权：应用 API 安全鉴权（access_key 签名），需先在控制台创建**应用/项目**并生成密钥
- 备选路径（若应用 API 受限）：OneNET **规则引擎 → 消息队列MQ / HTTP推送**

### 2.3 OneNET 物模型属性（需在控制台先建好，标识符必须一字不差）

| 标识符 | 名称 | 类型 | 单位 | 上报频率 |
|---|---|---|---|---|
| `temperature` | 温度 | float | ℃ | 每次唤醒（10 min） |
| `humidity` | 湿度 | float | %RH | 每次唤醒（10 min） |
| `rssi` | 信号强度 | int32 | dBm | 每次唤醒 |
| `seq` | 上报序号 | int32 | — | 每次唤醒 |
| `sensor_ok` | 温湿度有效标志 | bool | — | 每次唤醒 |
| `wake_reason` | 唤醒原因 | int32 | — | 每次唤醒 |
| `publish_failures` | 累计上报失败次数 | int32 | — | 每次唤醒 |
| `co2` | CO₂ 等效浓度 | int32 | ppm | **每 30 min** |
| `tvoc` | TVOC | int32 | ppb | **每 30 min** |

> ⚠️ OneNET 物模型模式下，报未定义标识符会被平台丢弃或报错。代码里报什么，控制台就必须先建什么。

### 2.4 一次唤醒周期

```
上电/定时唤醒
  → 读唤醒原因 esp_sleep_get_wakeup_cause()
  → seq++（RTC_DATA_ATTR，深睡保持）；co2_tick++
  → 【若本周期要采 CO2】先给 SGP30 上电（提前启动 15s 预热）
  → GPIO 给 DHT11 上电，等待 ~1.2s 稳定
  → 读 DHT11；失败则等 >2s 重读一次（DHT11 采样率 ≤1Hz）
  → 范围检查 0~50℃ / 20~90%RH → sensor_ok
  → 关 DHT11：DATA 设输入、关内部上拉、电源脚拉低
  → 连 Wi-Fi（硬超时 10s，失败即放弃）
  → 连 MQTT/TLS（硬超时 ~8s，失败即放弃）      ← 与 SGP30 预热并行
  → 【若本周期要采 CO2】读 SGP30（此时预热已在 Wi-Fi 期间走完），存 baseline
  → 组装 OneJSON，publish QoS1，等 PUBACK（硬超时 5s）
  → 断 MQTT → esp_wifi_stop()
  → 记录失败计数（RTC_DATA_ATTR）
  → 关 SGP30 电源
  → esp_sleep_enable_timer_wakeup(600s) → esp_deep_sleep_start()
```

**关键优化**：SGP30 的 15 秒上电预热与 Wi-Fi/TLS 连接**并行**，把预热时间藏进连接时间里，而不是额外等待。这是本设计对续航最大的单项优化。

### 2.5 关键设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 框架 | ESP-IDF 5.4.3 | 环境已就绪零下载；深睡/电源域/RTC 控制最精细 |
| MQTT 组件 | **复用 AloT `MY_MQTT` 并改造** | 已是 OneNET 物模型实现（TLS+CA+HMAC token+QoS1），改造量远小于新写 |
| 上报 QoS | **QoS1**（`mqtt.c:284` 原代码已是） | 项目书 1.3 节要求"服务端按序号检查缺失" |
| 自动重连 | `disable_auto_reconnect = true`（原代码已是） | 项目书：不能长时间重连，失败即睡眠 |
| **重连退避定时器** | **必须删除** | `mqtt.c:13-18 / 148-165 / 205-225 / 310-318` 是常供电架构的产物（5s→120s 退避），深睡节点必须删 |
| `onenet_start()` 语义 | 由**非阻塞**改为**带超时等待 `MQTT_EVENT_CONNECTED`** | 原实现靠轮询，深睡前会阻塞在不确定时长 |
| CO₂ 采样频率 | **每 30 分钟**（3 个唤醒周期） | SGP30 单次 48.8mA×15s ≈ 0.203mAh；10 分钟一次会额外增加 1.22mA 平均电流，直接击穿项目书 0.82mA 目标 |
| SGP30 基线 | 用 `sgp30_get_baseline`/`sgp30_restore_baseline` 存 `RTC_DATA_ATTR` | 规避官方要求的"连续运行 12 小时建立基线"；RTC 内存深睡保持、掉电清零 |
| PSRAM | **关闭** | 不需要；Octal PSRAM 占 GPIO33~37 且增加功耗。AloT 默认开着，**照抄是坑** |
| 时间戳 | 节点不带时间，由 OneNET 平台打 | 省去每次 NTP 的开销 |
| 凭据 | `app_secrets.h` + `.gitignore` 排除 | 项目书 1.1 节："Wi-Fi 密码不写入公开代码仓库"；新仓库是 **public**，这条是硬要求 |

### 2.6 不变量（Invariants）

1. **任何路径都必须最终进入深度睡眠**——包括传感器失败、Wi-Fi 超时、MQTT 失败、异常分支。不存在 `while(1)` 等待。
2. 所有阻塞调用必须有上界超时；**禁止 `portMAX_DELAY`**。
3. 深度睡眠期间不得有周期性串口输出、不得有活动定时器任务。
4. 传感器在睡眠前必须断电（或在实测确认 idle 电流可接受后常供电），且数据线不再被内部上拉拉高。
5. Wi-Fi 密码与 OneNET 密钥只出现在 `app_secrets.h`，且该文件被 `.gitignore` 排除。

---

## 3. 分步实施（每步都有可验证的产出）

| 步 | 内容 | 产出 | 通过条件 | 谁做 |
|---|---|---|---|---|
| **0** | 确认板子丝印版本、模组型号、COM 口、接线；OneNET 注册与实名认证 | `docs/HARDWARE.md` | 板子 v1.1/N8R8 已确认；接线表与 GPIO 无冲突 | 你提供信息 + 网页操作，我核对 |
| **1** | ★ **先写 `.gitignore`**；建 ESP-IDF 工程骨架；N8R8 配置（8MB flash、**SPIRAM 关闭**）；`idf.py build` + 烧录空 app | `firmware/` + `.gitignore` + 串口启动日志 | 编译 0 error；串口打印芯片型号/Flash 大小/复位原因；`git status` 不含 `app_secrets.h` | 我写，你插板 |
| **2** | 移植改造 DHT11（删 2 秒轮询任务，加电源门控 API），GPIO4/5 | `components/dht11` | 项目书调试步骤 2：连续 10 次读取有效；手握数值变化 | 我写 |
| **3** | 移植改造 `sgp30`（删 I2C 旧驱动依赖/加电源门控）；单测 CO₂/TVOC 读数 | `components/sgp30` | 能读到合理 eCO₂（400~1000ppm）与 TVOC；测出**真实预热时间** | 我写 |
| **4** | OneNET 控制台：建产品 + 设备 `node01` + 物模型 9 个属性；改造 `MY_MQTT`（删退避定时器、加连接/发布超时） | `components/onenet` + 控制台截图 | 串口看到 MQTT connected；控制台"设备数据流"出现属性值 | 你建控制台，我写代码 |
| **5** | 改造 `wifi_sta` 加硬超时（删 `portMAX_DELAY` 与 10s 重连定时器）；串通「采样→Wi-Fi→OneNET」全链路 | `components/wifi_sta` + 日志 | 控制台看到结构正确的 OneJSON，9 个标识符全部有值 | 我写 |
| **6** | 深度睡眠闭环：定时唤醒、RTC 序号、失败计数、唤醒原因、**关闭板载 RGB 灯** | 完整 `main.c` | 项目书调试步骤 4：串口每次重新启动；休眠期无持续输出；RGB 灯熄灭 | 我写 |
| **7** | 传感器电源门控 + **实测休眠电流与 VCC** | 电源门控代码 + 测量记录 | 项目书调试步骤 5 + 表 14：开发板休眠 <5mA；DHT11 休眠 VCC≈0V；记录 SGP30 常供电/门控两种方案的实测电流 | 我写，你测 |
| **8** | 鲁棒性：断网 / 关 OneNET 连接 / DHT 断线 / SGP30 无响应 四类故障注入 | 故障测试记录 | 项目书调试步骤 7 + 5.4 节：节点不卡死，下一周期自动上报 | 我写脚本，你拔网线 |
| **9** | 恢复 600s 周期，24h 连续试运行；用 `onenet_pull.py` 拉数据；`analyze_run.py` 统计 | 验收报告 | 项目书 1.3 节：采样成功率 ≥99%、上报成功率 ≥98%、周期误差 ±10s、故障后无需人工复位 | 我统计，你保持供电 |
| **10** | （第二阶段）电池 + 稳压 + 休眠电流优化；重算续航表 | 电源方案 + 实测 | 项目书表 9：开发板 <5mA；优化硬件 <100µA（理想 20~50µA） | 待第 9 步完成后再细化 |

> **进度（2026-10-11 更新）**
> 步骤 **0** 🟡（板子 v0.2/N8R8/COM6 已实测确认；OneNET 已建好，待核对 9 个属性与凭据）
> ｜ 步骤 **1** ✅ ｜ 步骤 **2** ✅（连续 14 次 CRC 通过；遗留"无外部上拉"待办，见 §4）
> ｜ 步骤 **4/5** 🟡 组件已写完并在真机跑通「DHT11 → Wi-Fi → TLS → MQTT 到达服务端」，
> **唯一阻塞 = OneNET 回 `CONNACK=4`**；PC 端已穷举 44 个变体全失败，须回控制台核对三元组
> ｜ 步骤 **3、6~10** ⬜
>
> 每步的原始实测证据在 `docs/WALKTHROUGH.md` 对应小节的「本机实测结果」表；硬件实测值汇总在 `docs/HARDWARE.md` §5。

---

## 4. 风险与对策

| 风险 | 后果 | 对策 |
|---|---|---|
| **OneNET 鉴权 `CONNACK=4`**（用户名或密码错误） | 链路全通但一条数据也上不去，且平台返回码是黑盒 | 已排除：签名算法 md5/sha1/sha256、密钥是否 base64 解码、4 个可用接入点、明文/TLS、`et` 五种取值、`res`/`sign` 编码四种组合、故意不存在的产品与设备、username 与 client_id 对调 —— **44 组全 4**。剩余唯一可能 = 控制台三元组与 `app_secrets.h` 不一致，须人工核对（见 `docs/ONENET_SETUP.md` §6.1） |
| **OneNET `client_id` 冲突**：AloT 与新节点都用同一产品凭据时互相踢下线 | "连上→秒断→重连"的随机故障，极难排查 | 已决策：新建独立产品 + 新设备 `node01`（用户 m00208） |
| **OneNET 物模型属性未先在控制台定义** | 上报被平台丢弃或报错，表现为"节点发出去了但平台没数据" | 步骤 4 强制先在控制台建 9 个属性；步骤 5 用控制台数据流逐字段核对 |
| **TLS 握手让单次唤醒从 4s 变成 6~8s** | 续航低于项目书表 9 估算 | 接受（安全优先）；步骤 9 实测唤醒耗时后重算续航表；`WIFI_TIMEOUT_MS`/`MQTT_TIMEOUT_MS` 需比项目书示例放宽 |
| **SGP30 峰值电流 48.8mA > ESP32 GPIO 输出上限（约 40mA）** | **不能用 GPIO 直驱给 SGP30 门控供电**（会拉低电压或损坏 IO） | 步骤 7 两条路线实测取优：①常供 3.3V 只在采样时发测量命令，实测 idle 电流；②GPIO + 外部 MOSFET/负载开关门控 |
| **SGP30 预热 15s + 基线 12h** 的不确定性 | 若预热无法缩短，续航与数据可信度都受影响 | 已设计"预热与 Wi-Fi 连接并行"；步骤 3 实测真实预热时间与 baseline 恢复效果 |
| **AloT `MY_MQTT` 含真实明文凭据**（`app_secrets.h`，内含 Wi-Fi 密码与 OneNET 密钥） | 新仓库是 **public**，误提交即真泄露 | 已核查：AloT 为私有仓库且真实文件从未被 git 跟踪，**目前未泄露**。步骤 1 **第一件事**就是写 `.gitignore`；提交前 `git status` 复核 |
| 板载 WS2812 RGB 灯未关（v1.0=GPIO48，v1.1=**GPIO38**） | 深睡电流上百 µA~数 mA，省下的功耗全丢 | 步骤 6 加熄灯代码并**实测验证**（GPIO38 这个映射来自搜索摘要，web_fetch 原页失败，须实测确认） |
| 板上红色电源 LED 硬连 3.3V，软件关不掉 | 休眠电流停在毫安级，达不到 <100µA | 项目书已说明；分两目标：先 <5mA（开发板），第二阶段换料 |
| **DHT11 模块上没有上拉电阻**（步骤 2 实测：自检报 `内部上拉读 1 / 内部下拉读 0`，而传感器照样能读数） | 数据线空闲期电平不确定；**上电冷启动后约 12 秒内 4 次尝试全部失败**（已实测）；线变长 / 受干扰 / 温差大时裕量更薄 | **用户决定（2026-10-11）：暂不加电阻，继续推进**。标准修法 = DATA↔3V3 加 4.7k~10k。**步骤 7 之前必须解决** —— 若启用 GPIO 门控给 DHT11 供电，每次唤醒都是一次冷启动，可能每次都读到失败。替代方案：拉长 `DHT11_POWER_SETTLE_MS` 并增加重读次数 |
| DHT11 若补上拉且启用 GPIO 门控，有经 DATA 线倒灌"幽灵供电"的风险 | 深睡时传感器 VCC 不为 0 | `dht11_safe_off()` 已强制关内部上拉 + 释放数据线（不变量 ④）；步骤 7 万用表实测 VCC |
| **GitHub 访问不稳定** —— ~~待查~~ **已定位并解决（坑 7）**：WinINET 代理 `127.0.0.1:7877`（MyClash）只被浏览器/`.NET` 读取，`git.exe`/`curl.exe` 不读 | `Failed to connect to github.com port 443 after 21093 ms` / `curl: (28)` | **已修**：仓库级 `git config http.proxy`+`https.proxy` = `http://127.0.0.1:7877` → `ls-remote` 1.03 秒返回。详见 `README.md`「提交与推送」 |
| 本机 git 无 user.name/email 与凭据 | 无法提交/push | 步骤 1 一并配置；凭据用 scoop 自带 `git-credential-manager.exe` 或 PAT |
| 24h 试运行期间 PC 休眠/断网 | 数据缺失被误判为节点故障 | 拉取脚本记录自身运行时间戳；关闭 PC 睡眠 |
| IDF 的 Wi-Fi 首次连接需 PHY 校准（1~3s、>100mA） | 单次唤醒时间与续航估算偏差 | 保留 PHY 校准数据存 NVS（默认开），第二次起复用；步骤 9 记录实际唤醒耗时 |

---

## 5. 停止条件（Stop Conditions）

遇到以下任一条，停下来向你确认，不自行继续：

1. 步骤 1 无法定位串口，或烧录失败且排除了接线/驱动问题的可能。
2. 步骤 0 发现 GPIO4/GPIO5/GPIO8/GPIO9 被板载外设占用，且你希望保留该外设。
3. 步骤 7 实测：传感器断电后 VCC 仍 >0.3V，**或** SGP30 常供电 idle 电流 >1mA 而门控又必须新增器件——需要决定是否加 MOSFET/负载开关（涉及新增器件与成本）。
4. 步骤 3 实测 SGP30 预热时间无法压到 20 秒内、且每 30 分钟采一次仍使续航跌破 30 天。
5. 步骤 9 验收指标不达标（采样 <99% 或上报 <98%），需要判断是硬件还是软件问题。
6. 需要改动项目书的既定指标（周期、量程、阈值）时。
7. OneNET 账户无法完成实名认证，或免费额度不足以支撑 24h 试运行。

---

## 6. 待现场确认（步骤 0 看板子与接线时一并确认）

1. 板子丝印版本 **v1.1**（用户已确认）→ 用于确认 RGB 灯在 GPIO38；**仍须实测点亮验证**。
2. 模组丝印 `ESP32-S3-WROOM-1-N8R8`（用户已确认）→ 步骤 1 用 `esptool flash_id` 复核。
3. 插上 **"UART"** 口后新增的 COM 口号（当前已知 COM3/COM4）。
4. **SGP30 的 I2C 引脚**：拟用 `SDA=GPIO8`、`SCL=GPIO9`（回避 DHT11 的 GPIO4/5、USB 的 GPIO19/20、绑带脚 GPIO0/3/45/46、Octal PSRAM 的 GPIO33~37）——待接线时确认板上无冲突。
5. **DHT11 电源门控引脚**：项目书定为 GPIO5（DHT11 约 1mA，GPIO 直驱安全）。
6. **SGP30 电源方案**：常供 3.3V 还是 GPIO+MOSFET——由步骤 7 实测决定。

---

## 7. DevKitC-1 针对性更正（相对项目书）

| # | 更正 | 理由 |
|---|---|---|
| 1 | 板载 WS2812 RGB 灯必须在深睡前关闭（v1.1=**GPIO38**，v1.0=GPIO48） | 常亮会吃掉全部省下的功耗，项目书 4.1 节明确要求关闭 |
| 2 | 必须用标着 **"UART"** 的 Type-C 口，不要用原生 "USB" 口 | UART 口经桥接芯片可自动复位烧录；原生 USB 在深睡唤醒时重新枚举会掉线 |
| 3 | 板上红色电源 LED 硬件直连 3.3V，**软件关不掉** | 这就是"开发板休眠 <5mA"这一宽松指标的原因；<100µA 必须靠第二阶段硬件改造 |
| 4 | GPIO4 / GPIO5 / GPIO8 / GPIO9 在本板上空闲；避开启动绑带脚 GPIO0/3/45/46、Flash 脚 GPIO26~32、Octal PSRAM 脚 GPIO33~37（N8R8 的 PSRAM 即使 `CONFIG_SPIRAM=n`，脚位也不宜复用） | 确认选脚安全 |
| 5 | **关闭 PSRAM**：`CONFIG_SPIRAM=n` | N8R8 有 8MB Octal PSRAM，但本应用不需要；AloT 默认开启，照抄会误开并增加功耗 |

---

## Plan Verification

- **Mode**: fact-check（fallback — logicprobe 插件未安装）
- **Claims checked（v1）**: 14 项 → 12 confirmed、0 refuted、2 unverifiable（行为性声明，降级人工确认）
- **Escalation**: 未执行模型级验证（需 logicprobe 插件）；涉及"超时/重试/深睡必然进入"的行为性声明降级为人工确认 + 步骤 8 实机故障注入验证

### v1 逐条核查

| # | 声明 | 证据 | 结论 |
|---|---|---|---|
| 1 | ESP-IDF 5.4.3 位于 `D:\esp\v5.4.3\esp-idf` | `git describe --tags` → `v5.4.3`；`components/esp_common/include/esp_idf_version.h` MAJOR=5/MINOR=4/PATCH=3；`export.ps1` True | confirmed |
| 2 | IDF 自带 esp-mqtt 组件 | `D:\esp\v5.4.3\esp-idf\components\mqtt` 存在 | confirmed |
| 3 | AloT DHT11 导出 `dht11_init` / `dht11_read` / `dht11_get_temperature` / `dht11_get_humidity` / `dht11_is_valid` / `dht11_start_task` | `components\DHT11\dht11.h:10-17` | confirmed |
| 4 | AloT DHT11 读取时关中断 | `components\DHT11\dht11.c:42` `portDISABLE_INTERRUPTS();`、`dht11.c:111` `portENABLE_INTERRUPTS();` | confirmed |
| 5 | AloT DHT11 无传感器供电门控 API | `dht11.h` 全部 6 个签名均无 power/enable 参数 | confirmed |
| 6 | AloT WIFI `wifi_init_sta` 无限等待 | `components\WIFI\wifi.c:126-127` `xEventGroupWaitBits(..., portMAX_DELAY)` | confirmed |
| 7 | AloT WIFI 有 10s 后台重连定时器 + 5 次快速重试 | `wifi.c:15-16` `WIFI_FAST_RETRY_MAX 5` / `WIFI_SLOW_RETRY_PERIOD_S 10`；`wifi.c:32-48` `wifi_start_slow_retry` | confirmed |
| 8 | AloT MY_MQTT 是 OneNET 专有（v1 据此判"不能用于本机 Mosquitto"） | `components\MY_MQTT\mqtt.c:188,193,198,392,432` 均为 `$sys/%s/%s/thing/...`；同目录 `onenet_token.c` | confirmed（结论已在 v2 随平台变更而反转） |
| 9 | Arduino IDE / arduino-cli / PlatformIO / Mosquitto 未安装 | `Get-Command arduino-cli,pio,mosquitto` 均为空 | confirmed |
| 10 | 可用串口 COM3、COM4 | `[System.IO.Ports.SerialPort]::GetPortNames()` → `COM4, COM3` | confirmed |
| 11 | VS Code 已装 espressif.esp-idf-extension-2.3.0 | `%USERPROFILE%\.vscode\extensions` 列表 | confirmed |
| 12 | AloT target=esp32s3、8MB flash、Octal PSRAM | `sdkconfig.defaults`：`CONFIG_IDF_TARGET="esp32s3"`、`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`、`CONFIG_SPIRAM_MODE_OCT=y` | confirmed |
| 13 | 改造后节点在断网时能按时回到深度睡眠 | 行为性声明，静态检查无法证明 | unverifiable → 步骤 8 实机故障注入 |
| 14 | GPIO5 直驱可在休眠时把传感器 VCC 拉到约 0V | 依赖外部上拉与板载电路，需实测 | unverifiable → 步骤 7 万用表验证 |

### v2 新增声明核查

| # | 声明 | 证据 | 结论 |
|---|---|---|---|
| 15 | AloT `MY_MQTT` 可直接对接 OneNET Studio（TLS 8883 + CA + HMAC token） | `mqtt.c:320-353`（hostname/port/SSL/CA）、`mqtt.c:152-157` token 生成、`mqtt.c:284` QoS1 | confirmed |
| 16 | AloT `MY_MQTT` 使用 **QoS 1** | `mqtt.c:284` `esp_mqtt_client_publish(...,0,1,0)` 第 5 参数=1 | confirmed |
| 17 | AloT `MY_MQTT` 已设 `disable_auto_reconnect = true` | `mqtt.c:320-353` 配置体内 | confirmed |
| 18 | AloT `MY_MQTT` 含需删除的重连退避定时器 | `mqtt.c:13-18` 退避状态、`mqtt.c:148-165` 回调、`mqtt.c:205-225` DISCONNECTED 分支、`mqtt.c:310-318` 创建 | confirmed |
| 19 | AloT SGP30 提供 baseline 存取 API | `components\SGP30\sgp30.h:19-22` `sgp30_get_baseline` / `sgp30_restore_baseline` | confirmed |
| 20 | OneNET 提供「设备属性记录查询」应用 API 供 24h 验收拉数据 | `https://open.iot.10086.cn/doc/iot_platform/book/api/application/queryDevicePropertyHistory.html` HTTP 200 | confirmed |
| 21 | SGP30 测量模式供电电流 48.8 mA | 搜索摘要转引 SGP30 datasheet 表 "Supply current in measurement mode" | confirmed（二手来源，步骤 7 实测复核） |
| 22 | SGP30 上电需约 15 秒才出有效读数 | **未取到原始 datasheet 页面**（web_fetch 超时） | **unverifiable → 步骤 3 实测** |
| 23 | 板载 WS2812 RGB 在 v1.1 为 GPIO38 | 搜索摘要（Adafruit circuitpython issue #9275 标题）；**web_fetch 该页失败** | **unverifiable → 步骤 6 实测点亮验证** |
| 24 | SGP30 的 48.8mA 峰值超过 ESP32 GPIO 单脚输出能力（约 40mA） | 由 #21 与 ESP32-S3 datasheet I/O 直流特性推导 | confirmed（推导，步骤 7 回避该接法） |
