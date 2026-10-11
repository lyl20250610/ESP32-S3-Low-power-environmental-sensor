# 软件架构 — ESP32-S3 超低功耗无线环境监测节点

> 这份文档描述**代码实际长什么样**，以及**接下来要长成什么样**。
> 实施顺序与验收标准见 [PLAN.md](PLAN.md)，逐步原理讲解见 [WALKTHROUGH.md](WALKTHROUGH.md)。

---

## 1. 一句话架构

> **这个固件不是一个"一直运行的程序"，而是"每 10 分钟被执行一次的脚本"。**

`app_main()` 就是一次完整的工作周期：醒来 → 采样 → 上报 → 断电 → 睡回去。
没有常驻任务，没有状态机循环，没有后台重连。整个架构的每个决定都从这一句推导出来。

---

## 2. 分层

```
┌─────────────────────────────────────────────────────────────┐
│  main/                       业务编排层（唯一有"决策"的地方） │
│  ├─ main.c            app_main() + APP_DEBUG_STAGE 阶段分支  │
│  ├─ app_config.h      全部可调参数集中一处（引脚/周期/超时） │
│  └─ app_secrets.h     凭据（.gitignore 排除，只有 .example） │
└──────────────────────────┬──────────────────────────────────┘
                           │ REQUIRES
┌──────────────────────────▼──────────────────────────────────┐
│  components/                 能力层（各自只知道自己的器件）   │
│  ├─ dht11/       ✅ 已完成   单总线温湿度，含电源门控         │
│  ├─ sgp30/       ⬜ 步骤 3   I²C eCO₂/TVOC，跨唤醒保存基线    │
│  ├─ wifi_sta/    ⬜ 步骤 5   一次性连接 + 硬超时（改自 AloT） │
│  └─ onenet/      ⬜ 步骤 4   OneNET 物模型上报（改自 AloT）   │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  ESP-IDF v5.4.3   driver / esp_wifi / mqtt(esp-mqtt) /       │
│                   esp_sleep / nvs_flash / esp_timer          │
└─────────────────────────────────────────────────────────────┘
```

**层间规矩**：能力层组件**不许**互相依赖，也不许知道"项目"的存在（不 include `app_config.h`）。
需要外部决定的参数，一律做成函数入参（例如 `dht11_read_once(data_pin, power_pin, …)`），
而不是让组件自己去读全局配置。这样每个组件可以单独测试、单独复用。

---

## 3. 配置与凭据：三个地方，互不重叠

| 文件 | 放什么 | 举例 | 入库？ |
|---|---|---|---|
| `main/app_config.h` | **业务与硬件参数** | `APP_SLEEP_SECONDS 600`、`APP_CO2_EVERY_N_WAKEUPS 3`、`APP_PIN_DHT_DATA GPIO_NUM_4`、`APP_WIFI_TIMEOUT_MS 10000`、`APP_DEBUG_STAGE 2` | ✅ |
| 组件自己的 `.h` | **协议级时序常量** | `DHT11_START_LOW_MS 20`、`DHT11_BIT_TIMEOUT_US 100` | ✅ |
| `main/app_secrets.h` | **凭据** | WiFi SSID/密码、OneNET 三元组 | ⛔ `.gitignore` |

分工理由：**"DHT11 起始信号要拉低 20ms" 是器件的物理性质，不是项目参数**，
调它不会让节点更省电，只会让它读不到数。所以它属于驱动，不属于 `app_config.h`。
（早期两边各写一份，成了双份真值来源，已删除重复项。）

`app_config.h` 里有编译期防御：

```c
#if !CONFIG_IDF_TARGET_ESP32S3
#error "…"      /* target 必须显式是 esp32s3 */
#endif
#if CONFIG_SPIRAM
#error "…"      /* PSRAM 必须关掉 */
#endif
```

——**配置错了就在编译期炸，不留到板子上现场找。**

---

## 4. 阶段开关：`APP_DEBUG_STAGE`

这是本项目调试策略的核心机制。步骤 2~5 **故意不深睡**（用 `vTaskDelay` 循环），
因为深睡会让串口变成"每次重启刷一大屏启动日志"，接线错误和代码错误根本看不出来。

| 值 | 阶段 | main.c 里的分支 |
|---|---|---|
| 1 | 工程骨架自检（芯片信息 + 深睡唤醒） | `stage1_sleep_demo()` |
| 2 | DHT11 连续读取（2 秒轮询，不睡） | `stage2_dht11_loop()` ← **当前** |
| 3 | SGP30 | 步骤 3 加入 |
| 4 | Wi-Fi + OneNET 上报（30 秒循环，不睡） | 步骤 5 加入 |
| 5 | 全链路（不睡） | 步骤 5 加入 |
| 6 | 深睡闭环 | 步骤 6 起 |

改一个宏 + 重编译就切换阶段；**每个阶段的代码都留在仓库里**，回头复现问题不用翻 git 历史。

---

## 5. 一次唤醒的完整流程（目标形态，步骤 6+）

```
定时器唤醒 / 上电冷启动
  │
  ├─ 读唤醒原因、RTC 里的 seq / 失败计数 / SGP30 基线
  ├─ 关掉板载 RGB 灯（GPIO38）
  │
  ├─ 【并行开始】SGP30 需要采样时（每 3 次唤醒一次）：此刻就发测量命令
  │      ↓ 让它自己预热 15 秒，CPU 不去等它
  ├─ DHT11：上电 → 等 1.2s → 读 → 失败等 2.1s 重读 → 断电 + 释放 DATA
  ├─ Wi-Fi：连接（超时 10s，无后台重连）
  ├─ OneNET：MQTT over TLS 连接（超时 8s）→ 发 OneJSON（QoS1，等 PUBACK，超时 5s）
  ├─ 取 RSSI
  ├─ 读回 SGP30 的 co2/tvoc（此时预热早就好了）→ 保存新基线到 RTC
  │
  ├─ 【任何一条分支失败，都汇聚到同一个出口】
  └─ 传感器断电 → esp_sleep_enable_timer_wakeup(600s) → esp_deep_sleep_start()
```

**"SGP30 预热与 Wi-Fi/TLS 连接重叠"是本设计对续航最大的单项优化**：
SGP30 上电后需要约 15 秒才给有效读数，而 Wi-Fi+TLS 连接本身要 6~8 秒。
串行做就是 `15 + 8`，重叠做就只需要 `max(15, 8)`。

**唯一出口原则**（不变量）：`esp_deep_sleep_start()` 只能有一个调用点，
所有失败路径都必须走到它。宁可这一轮不上报，也绝不能卡住——卡住 = 电池在 100mA 上白烧。

---

## 6. 数据上行：OneNET 物模型

节点只上报、不订阅。主题与载荷由 AloT 移植来的 `onenet` 组件负责：

```
主题   $sys/{ONENET_PRODUCT_ID}/{ONENET_DEVICE_NAME}/thing/property/post
载荷   {"id":"<自增>","version":"1.0","params":{
          "temperature":{"value":24.6}, "humidity":{"value":58.0},
          "rssi":{"value":-53}, "seq":{"value":127}, "sensor_ok":{"value":true},
          "wake_reason":{"value":4}, "publish_failures":{"value":2} }}
        （每 3 次唤醒才多带 "co2" / "tvoc"）
```

物模型共 **9 个属性**（`temperature` `humidity` `rssi` `seq` `sensor_ok`
`wake_reason` `publish_failures` `co2` `tvoc`），创建方法见
[ONENET_SETUP.md](ONENET_SETUP.md)。

相对项目书原文的两处有意偏离：
- **D1 主题**：项目书用自定义主题 `env/node01/telemetry`，改用 OneNET 物模型固定主题。
- **D2 载荷**：项目书用扁平 JSON，改用 OneJSON 的 `params/值` 嵌套格式。

时间戳**一律不带**——由平台按接收时刻打，避免节点没有 RTC 造成的漂移。

---

## 7. 构建体系

```
firmware/
├─ CMakeLists.txt        顶层：把 components/ 挂进构建 + project(env_node)
├─ sdkconfig.defaults    默认配置（只在首次生成 sdkconfig 时生效）
├─ partitions.csv        自定义分区表（nvs / phy_init / factory 2MB）
└─ build/                生成物，被 .gitignore 排除
tools/
├─ idf.ps1               激活 ESP-IDF 环境并转发给 idf.py（不声明 param()）
└─ idf.cmd               给 idf.ps1 加 -ExecutionPolicy Bypass 的外壳
```

`idf.cmd build` / `idf.cmd -p COM6 flash` 是**唯一推荐的构建入口**，
直接跑官方 `export.ps1` 在这台机器上是坏的（原因见 PLAN.md 0.3 的 6 个坑）。

关键编译配置（`sdkconfig.defaults`）：

| 配置 | 值 | 为什么 |
|---|---|---|
| `CONFIG_IDF_TARGET` | `"esp32s3"` | 不写会用默认 esp32 编不过 |
| `ESPTOOLPY_FLASHSIZE_8MB` | y | 模组是 N8R8（8MB Flash） |
| `# CONFIG_SPIRAM is not set` | 关 | **D6**：本项目只需几 KB JSON；AloT 默认是开的，照抄会误开，白耗功耗 |
| `# CONFIG_BT_ENABLED is not set` | 关 | 不用蓝牙，省体积 |
| `COMPILER_OPTIMIZATION_SIZE` | `-Os` | 体积换启动速度 |
| `PARTITION_TABLE_CUSTOM` | `partitions.csv` | 显式管 Flash 布局 |

---

## 8. 六条不变量（任何一次改动都不许违反）

| # | 不变量 | 它防的是什么事故 |
|---|---|---|
| ① | 每个网络操作都有超时上界 | 断网时节点卡死、电池被烧光 |
| ② | 睡眠前所有外设必须断电（含板载 RGB 灯） | 幽灵供电、漏电流 |
| ③ | 每个上报的属性必须先在控制台建物模型 | 平台静默丢弃数据 |
| ④ | 凭据绝不入库 | 仓库是 public |
| ⑤ | 失败时上报 `sensor_ok=false`，**不用上次的值冒充** | 数据造假，且看不出来 |
| ⑥ | 只有定时器/上电两种唤醒路径 | 睡下去再也醒不来 |

---

## 9. 现状与缺口

| 模块 | 状态 | 说明 |
|---|---|---|
| 工程骨架 / 构建 / 分区 / 烧录 | ✅ 已验证 | 编译 0 error 0 warning，已烧进板子 |
| `dht11` | ✅ 已验收 | 连续 14 次 CRC 通过（23.4℃ / 38.0%RH，0.1℃ 分辨率）。**遗留**：模块无外部上拉电阻，上电冷启动前约 12 秒会连读失败 —— 步骤 7 前须解决 |
| `sgp30` | ⬜ 未开始 | 移植 + 实测模块静态电流（决定常供还是门控） |
| `onenet` | ⬜ 未开始 | 从 AloT 移植，要删掉后台重连退避定时器 |
| `wifi_sta` | ⬜ 未开始 | 从 AloT 移植，要删掉 `portMAX_DELAY` 无限等待 + 10 秒后台重连定时器 |
| 深睡闭环 / 电源门控 | ⬜ 未开始 | 步骤 6/7 |
| `tools/onenet_pull.py` + `analyze_run.py` | ⬜ 未开始 | 24 小时验收统计用 |
| git 仓库 | ✅ 已打通 | 本地 = 远端 = `df065e3`；**坑 7**：本机 `git.exe`/`curl.exe` 不读 WinINET 代理，须仓库级 `http.proxy=http://127.0.0.1:7877` |

---

## 10. 复用 AloT 组件时的三个已知陷阱

参考工程：`D:\esp32\ESP32-S31\ESP32-S3\AloT`（同为 ESP-IDF，target esp32s3）

| 来源 | 陷阱 | 本项目处置 |
|---|---|---|
| `components/DHT11` | 带一个 **2 秒轮询常驻任务**；`dht11_get_temperature()` 不启动任务就恒返回 0 | 整个任务删掉，只留一次性 `dht11_read_once()` |
| `components/WIFI` | `xEventGroupWaitBits(..., portMAX_DELAY)` **无限等待** + 10 秒后台重连定时器 | 换成硬超时，删掉定时器（不变量 ①） |
| `components/MY_MQTT` | 已内建 5s→120s 指数退避重连定时器 | 删掉退避（一次唤醒只连一次）；其余 TLS/token/QoS1 直接复用 |
