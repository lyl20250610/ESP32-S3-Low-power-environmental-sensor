# 逐步讲解：ESP32-S3 低功耗环境监测节点

> 配套 `docs/PLAN.md`。本文回答"每一步到底在干什么、为什么这么干、怎么算成功、哪里会踩坑"。
> **硬件已确认**：ESP32-S3-DevKitC-1 **v1.1** + ESP32-S3-WROOM-1-**N8R8** + DHT11 3 针模块 +（新增）**SGP30**
> **平台已确认**：MQTT 走 **中国移动 OneNET**（TLS 8883），不再自建 Mosquitto
> 远端仓库：`https://github.com/lyl20250610/ESP32-S3-Low-power-environmental-sensor.git`

---

## 先修：五个必须先想明白的概念

不先理解这五点，后面每一步都会觉得"为什么非要这么麻烦"。

### 概念 1：为什么必须真·深度睡眠，不能用 `delay`

ESP32-S3 只要在运行，最低也要 20~30mA（关 WiFi、降频到 80MHz 也降不下去）；开着 WiFi 时 80~120mA。

如果程序这么写：

```c
while (1) {
    read_and_publish();
    vTaskDelay(pdMS_TO_TICKS(600000));   // 睡 10 分钟 —— 这是假的"睡"
}
```

这 10 分钟里芯片照样吃 20mA。3000mAh 电池按 70% 可用算 2100mAh，`2100 / 20 = 105 小时 ≈ 4.4 天`。

真正的深度睡眠：

```c
esp_sleep_enable_timer_wakeup(600ULL * 1000000ULL);
esp_deep_sleep_start();          // 芯片断电，只留 RTC 域，约 10µA
```

平均电流立刻掉到亚毫安级，续航从"天"变成"月"。

**代价必须认清**：深睡等于关机重启。RAM 全丢，程序从 `app_main` **从头再跑一遍**。所以：

- 每次唤醒都要重新初始化 WiFi / TLS / MQTT（这是整个功耗的大头）
- 程序的形态不再是"一个持续运行的系统"，而是"**每 10 分钟被执行一次的脚本**"

这个形态转变是本项目最核心的一次认知切换。

### 概念 2：`RTC_DATA_ATTR` —— 深睡中唯一活下来的东西

```c
RTC_DATA_ATTR uint32_t sequence_number = 0;   // 放在 RTC 慢速内存
static uint32_t temp = 0;                      // 放在普通 RAM，醒来是 0
```

- `RTC_DATA_ATTR` 的变量在深睡期间由 RTC 域供电，**下次醒来值还在**
- 普通全局变量醒来一律归零
- **拔电池彻底断电后，RTC 内存也会丢**，序号从 0 重来

最后一条很重要：24 小时验收里如果看到 `seq` 突然回到 0，那不是丢包，是节点被彻底断电了（拔线、Brownout）。要靠 `wake_reason` 区分。

本项目要用它存**四个**变量：`sequence_number`、`publish_failures`、`sensor_fail_count`，以及新增的 **`co2_tick`**（CO₂ 降频计数）和 **SGP30 基线**。

### 概念 3：为什么每个网络操作都必须有超时

电池节点的铁律：**宁可这一轮不上报，也绝不能卡住**。

卡住 = 芯片在 100mA 上白烧。项目书 4.1 节原话："路由器故障不能一直重试"。

所以设计成这样：

```
WiFi 连不上         → 最多等 10 秒 → 放弃 → 直接睡
TLS/MQTT 连不上     → 最多等  8 秒 → 放弃 → 直接睡
发布没收到 PUBACK   → 最多等  5 秒 → 放弃 → 直接睡
```

无论走哪条失败路径，**最后都必须汇聚到同一个 `enter_deep_sleep()`**。

这是 AloT 的 Wi-Fi 组件必须改的原因——`AloT\components\WIFI\wifi.c:127`：

```c
xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                    pdFALSE, pdFALSE, portMAX_DELAY);   // ← 永久等待
```

`portMAX_DELAY` 意思是"等到天荒地老"。再加上 `wifi.c:32-48` 那个 10 秒一次的后台重连定时器——这是给市电常供电设备写的，搬到电池节点上，路由器一坏，电池几小时就没了。

**同样的毛病在 OneNET 组件里也有**：`mqtt.c:13-18 / 148-165 / 205-225 / 310-318` 是一整套 5s→120s 的**重连指数退避定时器**，也必须删掉（好在 `disable_auto_reconnect = true` 原作者已经设了）。

### 概念 4：GPIO 能直接给传感器供电吗？—— 这里有个会导致返工的细节

能，但**有限度，而且 SGP30 越界了**。

| 器件 | 工作电流 | 能否 GPIO 直驱 |
|---|---|---|
| DHT11 | 约 **1~1.5mA**（待机 40~50µA） | ✅ 可以。项目书选 GPIO5 直驱是对的 |
| **SGP30** | 测量模式 **48.8 mA** | ❌ **不可以**。ESP32 GPIO 建议长期 ≤20mA、绝对上限 40mA |

**这是一条硬约束**：SGP30 峰值 48.8mA 超过了单个 GPIO 的承受能力。如果照 DHT11 的方式把 SGP30 的 VCC 接到 GPIO，轻则电压被拉垮传感器工作异常，重则打坏 IO。

所以 SGP30 的供电只有两条路（**步骤 3 和步骤 7 必须实测选优**）：

| 方案 | 做法 | 优点 | 代价 |
|---|---|---|---|
| **A. 常供电** | SGP30 直接接 3.3V，ESP32 睡它不睡 | 传感器状态不丢，**可能完全免掉 15 秒上电预热** | 传感器模块的静态电流变成 24 小时不间断开销 |
| **B. GPIO + MOSFET 门控** | GPIO 只做控制信号，MOSFET（如 AO3401 等 P 沟道）做开关 | 睡眠时真正断电 | 每次测量都要重新预热 15 秒，且新增器件 |

**关键判断**：如果 SGP30 模块的**静态（不测量）电流**是几十 µA 级别，那**方案 A 完胜**——省掉的 15 秒预热（48.8mA × 15s ≈ 0.2mAh/次）比那点静态电流值钱得多。

这个数字必须实测，不能猜。它可能改变整个硬件的接法。

### 概念 5：OneNET 的"物模型"是什么，为什么它约束了代码

项目书设计的是一条**扁平 JSON** 消息：

```json
{"node":"node01","seq":1234,"temperature_c":24.6,"humidity_rh":52.0,"rssi_dbm":-61,"sensor_ok":true,"wake_reason":4}
```

这套格式在自建 Mosquitto 上能跑，但**OneNET 不接受**。OneNET Studio 走的是"物模型"（OneJSON）：

```json
{"id":"msg0001","version":"1.0",
 "params":{"temperature":{"value":24.6},"humidity":{"value":52.0}}}
```

物模型的意思是：**平台先声明这个设备有哪些"属性"，设备才能上报这些属性**。

三条必须记住的规则：

1. **属性必须先在控制台建好**。你报一个没定义的标识符，平台会丢弃或报错——而节点那边看起来"发布成功了"（因为 broker 确实回了 PUBACK），**故障会非常隐蔽**。
2. **主题是平台规定的**，不能自己起名字：上报走 `$sys/{产品ID}/{设备名}/thing/property/post`。
3. **标识符拼写要完全一致**，`temperature` 和 `Temperature` 是两个东西。

本项目要在控制台建这 9 个属性（与 `PLAN.md` 2.3 节一致）：

| 标识符 | 类型 | 单位 | 频率 |
|---|---|---|---|
| `temperature` | float | ℃ | 10 min |
| `humidity` | float | %RH | 10 min |
| `rssi` | int32 | dBm | 10 min |
| `seq` | int32 | — | 10 min |
| `sensor_ok` | bool | — | 10 min |
| `wake_reason` | int32 | — | 10 min |
| `publish_failures` | int32 | — | 10 min |
| `co2` | int32 | ppm | **30 min** |
| `tvoc` | int32 | ppb | **30 min** |

---

## 针对 ESP32-S3-DevKitC-1 的五条更正

这是拿到具体型号后才明确的信息，会**修改项目书里的部分做法**：

### 更正 1：板载 RGB 灯在两代板上是不同引脚，必须实测确认

| 板子版本 | RGB LED (WS2812) 引脚 |
|---|---|
| ESP32-S3-DevKitC-1 **v1.0** | **GPIO48** |
| ESP32-S3-DevKitC-1 **v1.1** | **GPIO38** |

你的板子是 **v1.1**，所以代码里先按 **GPIO38** 写。

> ⚠️ **但这条我没能验证到原始出处**：消息来自搜索摘要（Adafruit circuitpython issue #9275），我尝试打开原页面时网络失败（`fetch failed`）。所以步骤 6 会**实际点亮它来验证**，不盲信。

这个灯如果不关，深睡时会一直亮，睡眠电流直接上百 µA 甚至几 mA——**辛辛苦苦省的功耗全被它吃掉**。项目书 4.1 节明确要求"关闭板载 RGB 灯"。

### 更正 2：两个 USB 口，必须用 "UART" 那个

DevKitC-1 有两个 Type-C：

- **"UART" 口**（经 USB-UART 桥接芯片）→ **用这个**
- **"USB" 口**（原生 USB，占 GPIO19/20）→ 不要用

原因：UART 口能通过 DTR/RTS 自动复位进下载模式，烧录稳定；而且深睡唤醒时设备会重新枚举，原生 USB 口会掉线，串口日志直接断掉，你会误以为节点死了。

### 更正 3：板上的红色电源指示灯关不掉

它硬件直连 3.3V 一路，**软件无法关闭**。这是"开发板休眠电流 <5mA"这个宽松指标的原因之一。想做到 <100µA 必须上第二阶段的硬件改造（换板或改电路）。

### 更正 4：引脚规划（含新增的 SGP30）

| 用途 | 引脚 | 说明 |
|---|---|---|
| DHT11 数据 | **GPIO4** | 空闲、非绑带脚 ✅ |
| DHT11 电源门控 | **GPIO5** | 空闲 ✅（DHT11 仅 ~1mA，可直驱） |
| **SGP30 SDA** | **GPIO8**（拟） | 待接线确认无冲突 |
| **SGP30 SCL** | **GPIO9**（拟） | 待接线确认无冲突 |
| **SGP30 电源** | **待定** | 3.3V 常供（方案 A）或 GPIO+MOSFET（方案 B）——见概念 4 |

**必须避开**：

- 启动绑带脚 GPIO0 / GPIO3 / GPIO45 / GPIO46
- 模组 SPI Flash：GPIO26~32
- **Octal PSRAM：GPIO33~37**（N8R8 有 Octal PSRAM，即使我们 `CONFIG_SPIRAM=n` 关掉它，脚位也不宜复用）
- 原生 USB：GPIO19 / GPIO20

### 更正 5：N8R8 的 PSRAM 要主动关掉（照抄 AloT 会踩坑）

你的模组是 **N8R8** = 8MB Flash + 8MB **Octal** PSRAM。

- Flash 要配：`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`
- **PSRAM 要关**：`CONFIG_SPIRAM=n`

为什么关：本项目最大的内存需求就是几个 JSON 字符串，**几 KB 就够了**，8MB PSRAM 一点用没有；而 Octal PSRAM 会占掉 GPIO33~37 并带来额外功耗。

⚠️ **AloT 的 `sdkconfig.defaults` 里 PSRAM 是开着的**（`CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y`）。直接复制那份配置过来会误开 PSRAM。**这是本项目最容易被忽略的一个配置坑。**

---

## 步骤 0：接线、硬件确认与 OneNET 注册

**目标**：把物理链路接对，把平台账号准备好。这一步不写任何代码。

**A. 硬件（你来做）**

1. 看板子**背面**丝印，记录版本（应为 v1.1）和模组型号（应为 `ESP32-S3-WROOM-1-N8R8`）
2. 按下面接线（**先只接 DHT11，SGP30 等步骤 3 再上**，一次只引入一个变量）：

   | DHT11 模块 | 接到 |
   |---|---|
   | VCC（或 +） | **GPIO5** |
   | DATA（或 OUT / S） | **GPIO4** |
   | GND（或 -） | **GND** |

   ⚠️ VCC **不接 3V3**，要接 GPIO5——因为后面要靠它给传感器断电。
3. 3 针模块**自带 4.7k~10k 上拉电阻**，不用再外接（比 4 针裸件省事，而且后面的"断电不倒灌"分析也依赖这个特性）
4. 用 **"UART" 那个 Type-C 口**插到电脑
5. 查串口：`[System.IO.Ports.SerialPort]::GetPortNames()`

**B. OneNET（你来做，我可以逐步指引）**

1. 注册/登录 <https://open.iot.10086.cn/>
2. **完成实名认证**（未实名无法接入设备）
3. 创建**产品**（建议命名 `环境监测节点`，协议选 MQTT）
4. 在产品下**创建设备 `node01`**，记下 **产品ID**、**设备名**、**设备密钥**
5. 按概念 5 的表，在产品"物模型"里建好 **9 个属性**

> **为什么必须新建产品/设备，不能直接用 AloT 那套？** MQTT 协议规定同一个 `client_id` 只能有一个连接。AloT 用的 `client_id` 是它的设备名，如果两边共用同一套凭据同时在线，两个设备会**互相踢下线**，表现为"连上→秒断→重连"的随机故障，极难排查。所以这个项目要有自己独立的产品和设备。

**为什么**

引脚接错，后面所有代码都白写；而 GPIO5 供电这一步如果一开始就接到 3V3，到步骤 7 要拆线重接，容易把模块焊盘弄坏。所以**一次接对**。

**验证**

- 串口列表里多出一个新的 COM 口（当前已知 COM3、COM4）
- 接线表拍照/记录，与上表一致
- OneNET 控制台里能看到设备 `node01`，状态"未激活"（正常，还没连过）

**坑**

- DHT11 模块的丝印顺序不统一，有的是 `VCC-DATA-GND`，有的是 `DATA-VCC-GND`，还有 `S-DATA` 混用。**只信模块上印的字，不信卖家图**。
- VCC/GND 接反，DHT11 会发烫，不一定会立刻坏，但读数会一直是 0。
- 杜邦线虚接是最常见的"玄学故障"。读数不稳先拔下来重插，别先怀疑代码。
- OneNET 实名认证是人工审核，**可能不是即时通过**，建议现在就去提交。

**这一课学到的**：嵌入式项目里，"接线正确"和"账号可用"都是需要显式验证的前提，不是默认成立。

---

## 步骤 1：`.gitignore` + 工程骨架 + 烧录链路

**目标**：拿到一个能编译、能烧录、能在串口看到日志的空工程，且**凭据绝不会被提交**。

**做什么**

1. ★ **第一件事：写 `.gitignore`**，在写任何代码之前：

   ```gitignore
   # 凭据 —— 必须排除，仓库是公开的
   **/app_secrets.h
   # ESP-IDF 构建产物（几万个小文件，不排除会把仓库撑爆）
   **/build/
   sdkconfig
   sdkconfig.old
   managed_components/
   dependencies.lock
   # 本地产物
   *.log
   __pycache__/
   ```

2. 在 `D:\esp32\xiangmu1\firmware\` 建 ESP-IDF 工程
3. 写 `sdkconfig.defaults`：

   ```
   CONFIG_IDF_TARGET="esp32s3"
   CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
   # CONFIG_SPIRAM is not set      ← N8R8 的 PSRAM 主动关掉
   CONFIG_PARTITION_TABLE_CUSTOM=y
   ```

4. 写一个**最小 `main.c`**：打印芯片型号、Flash 大小、PSRAM 状态、复位原因，然后进深睡
5. 配置 git（`user.name` / `user.email`）并关联远端仓库
6. `idf.py build` → `idf.py -p COMx flash monitor`

**为什么**

- **`.gitignore` 为什么排第一**：你的 AloT 工程里 `components\MY_MQTT\app_secrets.h` 含**真实明文**的 WiFi 密码和 OneNET 设备密钥。这个新仓库是**公开**的。我先核查过 AloT 那边是私有仓库且真实文件从未被 git 跟踪，**目前没有泄露**——但把凭据文件搬到公开仓库时，只要 `.gitignore` 晚写一步就真泄露了。
- **工程骨架先通**：任何嵌入式项目的第一步都应该是"**编译→烧录→看日志**这条链路先通"。跳过这步直接写业务代码，一旦出问题，你分不清是**代码写错了**还是**环境配置错了**——这是初学者最常卡住的地方，能卡好几天。

**验证**

- `idf.py build` 零 error
- 串口打印出真实的芯片型号、Flash 大小、复位原因
- `esptool flash_id` 读出的 Flash 大小与丝印 `N8R8` 对得上
- **`git status` 输出里绝对不出现 `app_secrets.h`**（这是本步的硬性验收）

**本机实测结果（2026-10-08）**

| 检查项 | 实测值 | 结论 |
|---|---|---|
| `idf.py build` | `Project build complete.` **退出码 0**，1047 个目标全部通过 | ✅ |
| 固件体积 | `env_node.bin` = **179,376 字节**（0x2BCB0），2MB app 分区里剩 91% | ✅ |
| Bootloader | 0x5760 字节，剩 32% | ✅ |
| `CONFIG_IDF_TARGET` | `"esp32s3"` | ✅ |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `"8MB"` | ✅ |
| `CONFIG_SPIRAM` | **`# CONFIG_SPIRAM is not set`** | ✅ 决策 D6 生效 |
| `CONFIG_BT_ENABLED` | `# CONFIG_BT_ENABLED is not set` | ✅ |
| `CONFIG_ESPTOOLPY_FLASHMODE_QIO` | `=y`，且 `CONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT=y`（烧录时再实测确认真实模式） | ✅ |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | `160`（步骤 10 调优时再比较 80MHz） | ⚠️ 待调优 |
| 分区表 | `CONFIG_PARTITION_TABLE_CUSTOM=y`，`CUSTOM_FILENAME="partitions.csv"` | ✅ |
| `git check-ignore app_secrets.h` | 命中 `.gitignore:8:app_secrets.h` | ✅ 硬性验收通过 |
| `git check-ignore build/env_node.bin` | 命中 `.gitignore:13:**/build/` | ✅ |

**⚠️ 关于串口的一个重要发现**：`[System.IO.Ports.SerialPort]::GetPortNames()` 列出的 `COM3`/`COM4`
经 `Win32_PnPEntity` 查证**都是"蓝牙链接上的标准串行"**（`BTHENUM\...`），**不是 ESP32**。
也就是说：**开发板当前没插、或插的不是数据口**。等你把板子插到标着 **"UART"** 的那个 Type-C 口后，
会**新出现**一个 COM 口（大概率是 COM5 或更大），烧录必须用那个新口，**绝不能填 COM3/COM4**。
（顺带一提：参考工程 AloT 的 `.vscode/settings.json` 里写的 `"idf.portWin":"COM3"` 就是踩了这个坑。）

**坑**

- **深睡一进去串口就彻底静默**，很容易误判为死机。所以最小 `main.c` 要**先打印、再延时几秒、最后才睡**。
- 烧录必须指定 `-p COMx`，且板子要插着。
- Windows 上如果串口被 `idf.py monitor` 占着，另一个终端烧录会报"拒绝访问"。先 Ctrl+] 退出 monitor。
- **本机访问 GitHub 不稳定**（实测第二次 `git ls-remote` 报 `curl 28 Recv failure: Connection was reset`）。push 失败就重试；持续失败就改用 VS Code 内建 Git，或配好凭据助手后走 SSH。
- 本机 `user.name` / `user.email` / `credential.helper` **全都没配**，第一次提交会失败，必须先配。

**这一课学到的**：把"环境问题"和"代码问题"隔离开是调试效率的关键；而**密钥保护是"提交前"的动作，不是"提交后"能补救的**。

### 1.1 本机实测踩到的 5 个环境坑（已全部绕过，记录备查）

这一步在本机实际卡了很久，卡点**全部是环境问题、跟项目代码无关**。这正是"先打通骨架"的价值——下面 5 个坑如果留到步骤 4 写 MQTT 时才撞上，你会以为是 OneNET 配置错了。

| # | 症状 | 真正原因 | 解法 |
|---|---|---|---|
| 1 | `idf.py : 无法将"idf.py"项识别为 cmdlet…` | 本机 PATH 上的 `python` 是 **Microsoft Store 的占位程序**（`…\WindowsApps\python.exe`），它跑不了 IDF 的 `activate.py`，于是官方 `export.ps1` 第 24 行拿到空值、第 27 行抛 `BadExpression`，工具链根本没进 PATH | 先把 `C:\Espressif\python_env\idf5.4_py3.11_env\Scripts` 放到 PATH **最前面**，再点源 `export.ps1` |
| 2 | `找不到 ESP-IDF：build\tools\idf.py 不存在`；`-p` 报"未知参数" | 包装脚本里声明了 `param()`，PowerShell 先把 `build` 绑到第一个位置参数，再把 `-p` 当它自己的参数 | 包装脚本**完全不声明 `param()`**，用 `$args` 原样转发 |
| 3 | 中文报错变 `鎵句笉鍒?IDF…`，甚至 `ParseError` | Windows PowerShell **5.1** 读**无 BOM** 的 UTF-8 `.ps1` 时按 GBK 解码 | `.ps1` 必须存成 **UTF-8 with BOM**（前 3 字节 `EF BB BF`） |
| 4 | `NativeCommandError`，位置 `export.ps1:24`，之后没有任何输出，退出码 1 | `activate.py` 往 **stderr** 打 `Activating ESP-IDF 5.4`；脚本里 `$ErrorActionPreference='Stop'` 时，`2>&1 \| Out-Null` 会把这条 stderr 当成 ErrorRecord **终止整个脚本** | 点源 `export.ps1` 时临时 `$ErrorActionPreference='Continue'` 并 `*> $null` |
| 5 | `无法加载文件 …\x.ps1。未对文件进行数字签名…` | `Set-ExecutionPolicy -List` 明明是 `RemoteSigned`，但 `D:\esp32\xiangmu1\` **及其子目录**下的 `.ps1` 一律被拦，而 `$env:TEMP`、`D:\esp`、`D:\esp32`（本级）下的同名文件正常 → 是**路径相关**、不是内容相关 | 显式放行：`powershell -ExecutionPolicy Bypass -File .\tools\idf.ps1 …`；或直接用 `tools\idf.cmd` |

**结论：以后一律用本工程自带的包装脚本**，不要直接跑 ESP-IDF 的 `export.ps1`：

```powershell
.\tools\idf.ps1 build                    # 编译
.\tools\idf.ps1 -p COM4 flash            # 烧录
.\tools\idf.ps1 -p COM4 monitor          # 看日志（Ctrl+] 退出）
.\tools\idf.ps1 -p COM4 flash monitor    # 烧完直接看日志
.\tools\idf.cmd build                    # 上面那条被执行策略拦时的替代写法
```

---

## 步骤 2：DHT11 驱动（先不上深睡）

**目标**：稳定读到温湿度，能连续打印。

**做什么**

1. 把 `AloT\components\DHT11\` 复制成 `firmware\components\dht11\`
2. **删掉常驻任务**：`sensor_dht11_task`、`dht11_start_task()`（那是个 2 秒轮询任务）
3. 新增"一次完整读取"的接口：`dht11_read_once(gpio_num_t data_pin, gpio_num_t power_pin, float *t, float *h)`
4. `main.c` 里写一个**延时循环**（每 2 秒读一次并打印）——**这一步故意不深睡**
5. 时序按项目书：电源脚拉高 → 等 1.2 秒稳定 → 读 → 失败则等 >2 秒重读一次

**为什么**

- **删任务**：AloT 那个轮询任务是为常供电设备写的。深睡架构下每次唤醒都会重建任务，白白浪费 RAM 和启动时间，而且它会和我们的时序打架。
- **先不深睡**：调试期需要连续观察读数变化才能判断接线对不对。每 10 分钟才打一行字，根本没法调试。
- **等 1.2 秒**：DHT11 上电后需要一个稳定过程；且它采样率上限是 1Hz，读太快会失败。
- **失败重读一次**：DHT11 用单总线协议，靠微秒级时序。AloT 的实现 `dht11.c:42` 用 `portDISABLE_INTERRUPTS()` 关中断来保证精度（约 4ms），但任何意外抢占仍可能失败。重读一次是标准做法。

**验证（对应项目书调试步骤 2）**

- 连续 10 次读数全部通过 CRC 校验
- 温度在合理范围（15~35°C），湿度 20~90%
- **用手握住传感器**，读数明显上升 → 证明是真实测量，不是假数据

**本机实现（代码已写完、编译零警告通过；真实读数待烧录实测）**

与 AloT 原版的差异都写在 [`firmware/components/dht11/dht11.h`](../firmware/components/dht11/dht11.h) 的文件头注释里。

| 文件 | 作用 |
|---|---|
| [`firmware/components/dht11/dht11.h`](../firmware/components/dht11/dht11.h) | 对外只有 3 个函数：`dht11_read_once()` / `dht11_release_data_pin()` / `dht11_safe_off()`。协议级时序常量也在这个头文件里：`DHT11_START_LOW_MS`(20) / `DHT11_POWER_SETTLE_MS`(1200) / `DHT11_RETRY_DELAY_MS`(2100) / `DHT11_BIT_TIMEOUT_US`(100) / `DHT11_BIT_ONE_MIN_US`(40) |
| [`firmware/components/dht11/dht11.c`](../firmware/components/dht11/dht11.c) | 单出口 + 关中断必开中断；新增两道 AloT 没有的防线 |
| [`firmware/main/main.c`](../firmware/main/main.c) | `APP_DEBUG_STAGE == 2` 分支：每 2 秒读一次并打印，不深睡；每 10 次打一条小结 |
| [`firmware/main/app_config.h`](../firmware/main/app_config.h) | 新增 `APP_DEBUG_STAGE` 阶段开关、`APP_DHT_POWER_GATE_ENABLED`(0 = 常供 3.3V)、`app_dht_power_pin()` |

驱动里新增的两道防线（AloT 原版没有）：

1. **全 0 判定**：`(data[0]|data[1]|data[2]|data[3]|data[4]) == 0` 直接判失败。因为"线没接"时收到的就是 5 个 0 字节，而 `0+0+0+0 == 0` 会让**校验和恰好通过** —— 这是单总线最常见的假成功陷阱。
2. **物理合理性边界**：超范围直接丢弃，绝不用上次的值伪装。

另外把"断电"做成了**强制动作**：`dht11_read_once()` 无论成功失败、走哪条错误分支，返回前都执行 `dht11_safe_off()`（断电 + 把数据线设成输入且**关闭内部上拉**）。关内部上拉的理由是不变量 ④：传感器断电后若本芯片仍往 DATA 线拉高，电流会经模块上拉电阻 + 传感器内部保护二极管倒灌进它的 VCC 轨，形成"幽灵供电"。

**本机实测结果 · 第一次（2026-10-08，DHT11 尚未接线）**

| 检查项 | 实测值 | 结论 |
|---|---|---|
| `idf.py build` | `Project build complete.` **退出码 0** | ✅ |
| `env_node.bin` | **180,256 字节**（0x2C020），2MB app 分区剩 91% | ✅ |
| 新文件警告数 | `dht11.c` / `main.c` / `app_config.h` **零警告**（`-Wall -Wextra -Werror=all`） | ✅ |
| `dht11` 组件被识别 | 构建日志 `Components:` 列表里出现 `dht11`，路径 `D:/esp32/xiangmu1/firmware/components/dht11` | ✅ |
| 烧录（`-p COM6 flash`） | `Wrote 178736 bytes (102126 compressed) at 0x00010000 in 2.4 seconds` / `Hash of data verified.` / **退出码 0** | ✅ |
| 启动日志完整性 | 芯片/Flash/PSRAM/复位原因/唤醒原因/RTC 计数/`APP_DEBUG_STAGE = 2` 全部打印，中文正常 | ✅ |
| 读取循环节奏 | 每 **3350 ms** 一轮（1200 ms 等待 + 读取 + 2100 ms 重读），与设计一致 | ✅ |
| 失败语义 | 两次都失败 → `本次读取最终失败：ESP_ERR_TIMEOUT（调用方应置 sensor_ok=false，不要用上次的值）`，**没有拿旧值冒充** | ✅ |
| 三阶段诊断 | 稳定报 `阶段1：传感器没有把总线拉低（没接 / 没供电 / DATA 接错脚）` | ✅ |
| **接线自检（关键）** | `DATA 线自检：内部上拉时读到 1，内部下拉时读到 0` → `⚠️ 没有外部上拉，DATA 线基本悬空` | ⛔ **当时 DHT11 确实未接线** |

**本机实测结果 · 第二次（2026-10-11，DHT11 已接线）→ 步骤 2 验收通过**

| 检查项 | 实测值 | 结论 |
|---|---|---|
| **连续 CRC 通过（验收主指标）** | 第 3 次起**连续 14 次全部通过**（第 3~16 次），零校验和错误 | ✅ **达标**（要求连续 10 次） |
| 稳定原始帧 | `26 00 17 04 41` → H=38.0 %RH，T=23.4 ℃。**手算校验和** `0x26+0x00+0x17+0x04 = 0x41` — 与第 5 字节完全一致 ✅ | ✅ |
| 首读原始帧 | `30 00 17 04 4B` → H=48.0 %RH；校验和 `0x4B` ✅ | ⚠️ 上电首读湿度偏高 10 %，DHT11 已知特性，**不可用于验收** |
| 温度分辨率 | 温度小数位恒为 `0x04` → 这颗是 **0.1 ℃ 分辨率变种**（标准 DHT11 小数位恒 0） | ✅ 优于项目书假定 |
| 读数合理性 | 23.4 ℃ / 38.0 %RH，落在 -20~60 ℃ / 0~100 %RH 内 | ✅ |
| 单次读取耗时 | **1224 ms**（其中 1200 ms 是 `DHT11_POWER_SETTLE_MS`） | 见「待优化」 |
| 读取周期 | 每 **3224 ms** 一轮（1224 ms 读取 + 2000 ms 间隔） | ✅ 与设计一致 |
| 升温验收 | 手持传感器后温度明显上升（用户现场确认） | ✅ |
| **上拉电阻自检** | 仍报 `内部上拉时读到 1，内部下拉时读到 0` → **DATA 线上确实没有外部上拉电阻** | ⚠️ 见下 |

**本次验收暴露的两个新问题（已定级，不影响步骤 2 结论）**

**问题 ①｜模块上没有 4.7 kΩ 上拉电阻（自检确证）**

`stage2_probe_data_line()` 的逻辑经复核无误：内部上拉读 1、内部下拉读 0，只能是外部阻抗远大于 ESP32 内部的约 45 kΩ。结论——**这颗 3 针模块上其实没有焊上拉电阻**（很多便宜模块只焊了电源 LED），现在完全靠 ESP32 内部那约 45 kΩ 硬撑。

- 能撑住的原因：杜邦线短、速率低。
- 风险：线变长、受干扰、温度变化时裕量变薄，会出现间歇性读取失败。
- **用户决定（m01003）**：现在不加电阻，继续推进。已记入 `docs/PLAN.md` 风险表与 `docs/HARDWARE.md` 待办。

**问题 ②｜上电冷启动后约 12 秒内 4 次尝试全部失败**

```
t=1.64s  FAIL ─┐
t=3.78s  FAIL ─┴─ 第 1 次（含一次 2100 ms 重读）
t=7.01s  FAIL ─┐
t=9.13s  FAIL ─┴─ 第 2 次
t=12.36s OK   ← 从此 14 次连续成功
```

日志首行是 `rst:0x1 (POWERON)` — **上电冷启动**，DHT11 与 ESP32 同时上电。这与问题 ① 是一件事的两面：没有外部上拉时，数据线在空闲期处于不确定状态，传感器上电阶段更容易漏掉起始信号。

**这条对后续步骤有直接后果**：如果步骤 7 采用「GPIO 门控给 DHT11 供电」（`APP_DHT_POWER_GATE_ENABLED 1`），那么**每 10 分钟唤醒都是一次冷启动**，可能重现这十几秒的失败 —— 而每 10 分钟只有一次采样机会，等于每次都上报 `sensor_ok=false`。**步骤 7 开工前必须先解决这件事**（加电阻，或把 `DHT11_POWER_SETTLE_MS` 拉长并增加重读次数）。

**待优化（不是错误，记在这里免得忘）**

`dht11_read_once()` 里的 `vTaskDelay(DHT11_POWER_SETTLE_MS)`（1200 ms）目前是**无条件**执行的，即使 `power_pin < 0`（常供、无门控）也要等满 1200 ms。在真实工作周期里这会让单次唤醒多花 1.2 s 的清醒时间。步骤 5/6 接入深睡后，应改成：**只有真正做了电源门控才等**；常供时改为只等一个短延时（≥1 s 的 DHT11 采样周期要求）。

**「接线自检」这次立了大功**：它在 4 条错误可能（没接 / 没供电 / 接错脚 / 固件坏）里直接排除了两种，把问题锁死在**硬件接线**上——不是固件的问题。这就是步骤 2 加 `stage2_probe_data_line()` 的价值：**没有它，看到"读取一直失败"你第一反应会怀疑驱动代码，可能要花几小时重新读时序代码。**

**本步骤撞到的新坑（构建系统，不是环境）：`fatal error: esp_flash.h: No such file or directory`**

- 原因：`idf_component_register()` 的 `REQUIRES` 一写内容，就**不再**享有"没写 REQUIRES 时自动依赖全部组件"的旧行为。步骤 1 之所以能白拿 `esp_flash.h` / `esp_chip_info.h` / `esp_app_desc.h`，正是因为那时 REQUIRES 是空的；步骤 2 为了用 `dht11` 写上 `REQUIRES dht11`，这些同时被切断。
- 修法：`main/CMakeLists.txt` 改成 `REQUIRES dht11` + `PRIV_REQUIRES esp_hw_support esp_app_format spi_flash`（分别对应 `esp_sleep.h`/`esp_chip_info.h`、`esp_app_desc.h`、`esp_flash.h`）。
- 教训：**报错最后那句 "add X to PRIV_REQUIRES list" 就是答案，照做即可**；看到 `fatal error: xxx.h: No such file or directory` 而该头文件确实存在于 IDF 里时，先怀疑 REQUIRES，别怀疑 IDF 装坏了。

**坑**

- 读数一直是 `0.0` 或全 1：几乎一定是上拉电阻或接线问题
- 读数间歇性失败：正常。DHT11 本身就是这样，靠重读兜住
- **温度小数位永远不是 `.0` 而是像 `4`**：说明这颗是 0.1 ℃ 分辨率变种，**不是 bug**；反过来标称 DHT11 的小数位恒为 0，两者都正常
- **`⚠️ 没有外部上拉` 这条自检有两种含义，别只想到一种**：① DATA 真的没接（第一次实测就是这种）；② **接了，但模块上根本没焊上拉电阻**（第二次实测是这种，此时传感器照样能靠 ESP32 内部 45 kΩ 工作）。判断方法：看**能不能读到数** —— 读不到 = ①，读得到但偶发失败 = ②。这条教训很值钱：自检的"悬空"结论只说明**线路阻抗高**，不等于"没接传感器"
- **加电冷启动后头十几秒可能连续失败**：DHT11 上电需要稳定时间，无外部上拉时更明显；稳定运行中不会出现
- 关中断 4ms 会影响系统实时性。我们的流程是"先读传感器，再连 WiFi"，两者不重叠，所以不冲突

**这一课学到的**：单总线传感器靠时序吃饭，容错设计（重读、范围校验）不是可选项。

---

## 步骤 3：SGP30 驱动 + 实测预热时间与静态电流

**目标**：读到合理的 eCO₂/TVOC，并**用实测数据决定 SGP30 的供电方案**。

**做什么**

1. 把 `AloT\components\SGP30\` 复制成 `firmware\components\sgp30\`
2. 该组件用的是旧版 I2C 驱动（`driver/i2c.h`），按 IDF 5.4.3 是否仍兼容决定迁移到 `driver/i2c_master.h` 还是保留
3. 接线：SDA→GPIO8、SCL→GPIO9、VCC→**先接 3.3V 常供（方案 A）**、GND→GND
4. 写测试程序：上电后**每秒记录一次 CO₂/TVOC 读数与时间戳**，打印出来
5. 从这份日志里读出三个关键数字：

   | 要测的量 | 怎么测 | 为什么重要 |
   |---|---|---|
   | **读到有效读数需要多久** | 从 `sgp30_init` 到第一次出现合理值（400~1000ppm）的秒数 | 决定"预热能不能藏进 WiFi 连接里"，也决定方案 B 的代价 |
   | **静态电流** | 万用表串联在 SGP30 的 3.3V 供电路径上，**不测量时**读数 | 若为几十 µA → 方案 A 完胜；若为 mA 级 → 必须上 MOSFET |
   | **测量时的峰值电流** | 同上，测量瞬间读数 | 验证是否真的 48.8mA，以及 GPIO 直驱是否真的不可行 |

6. 用 `sgp30_get_baseline` 读出基线，用 `sgp30_restore_baseline` 恢复，观察恢复后读数是否更快稳定

**为什么**

SGP30 有两个和深睡架构冲突的特性，**都必须实测而不是相信文档**：

- **上电预热**：官方说约 15 秒首帧有效。15 秒 × 48.8mA ≈ **0.2mAh/次**。如果每 10 分钟采一次，等于凭空增加 **1.22mA** 平均电流——**这一项单独就超过项目书"优化硬件后 0.82mA"的整机目标**。所以本项目决定 **CO₂ 每 30 分钟采一次**（用 `co2_tick` 每 3 个周期采 1 次）。
- **12 小时基线**：官方要求在连续运行约 12 小时后读数才准确。但 `sgp30.h:19-22` 提供了 baseline 存取 API，正好可以把这个基线存进 `RTC_DATA_ATTR` 跨唤醒保持，规避每次重新标定。

**并行优化（本设计的核心技巧）**：

```
唤醒 → 立刻给 SGP30 上电（预热开始）
     → 读 DHT11                      ← 预热在后台走
     → 连 WiFi + TLS + MQTT（6~8s）  ← 预热的绝大部分被藏在这里
     → 读 SGP30（此时预热已完成）
     → 上报
```

这样 15 秒预热**不额外增加墙钟时间**（只是这些时间里电流高了），是本设计对续航最大的单项优化。

**验证**

- CO₂ 读数在 400~1000ppm 合理区间（对着传感器呼气应显著上升）
- TVOC 有非零读数
- 三个关键数字都记录成表格

**坑**

- **SGP30 的 eCO₂ 不是真 CO₂**！它是从 TVOC 推算的"等效二氧化碳"，和 NDIR 传感器（SCD41/MH-Z19）测的物理 CO₂ 不是一回事。放在通风房间能大致反映趋势，**不能当精确 CO₂ 计用**。真值需求请换 SCD41。
- **绝对不要用 MQ-135 当 CO₂ 传感器**：大量网上教程这么写是错的。MQ-135 输出的是"空气质量"综合值，对 CO₂ 没有选择性，给不出 ppm；而且要 5V、150mA 加热、预热 24 小时。它会**同时破坏低功耗和测量可信度**。
- SGP30 模块（带 LDO 和电平转换的成品小板）的静态电流与裸芯片手册数字**不一定相同**，所以必须连模块一起测。
- 不要关内部上拉去读 I2C——I2C 需要上拉才能工作。SGP30 模块一般自带 I2C 上拉。

**这一课学到的**：一个"低功耗项目"里，**外设的功耗可能比主控还大**。选型时必须先算外设的账。

---

## 步骤 4：OneNET 控制台 + 改造 `MY_MQTT` 组件

**目标**：串口里看到 `MQTT connected`，控制台"设备数据流"里出现属性值。

**做什么**

**A. 控制台（步骤 0 已建好产品/设备/物模型，这里只核对）**

确认 9 个属性的标识符与 `PLAN.md` 2.3 节完全一致。

**B. 复制并改造 `AloT\components\MY_MQTT\` → `firmware\components\onenet\`**

这个组件已经是完整的 OneNET 实现，**改造量比新写小得多**。原代码里已经做对的部分：

| 位置 | 现状 | 评价 |
|---|---|---|
| `mqtt.c:320-353` | `studio-mqtts.heclouds.com` + 8883 + `MQTT_TRANSPORT_OVER_SSL` + 硬编码 CA | ✅ 直接可用 |
| `mqtt.c:152-157` | `dev_token_generate(..., SIG_METHOD_SHA256, 2524608000, ...)` HMAC 签名 | ✅ token 有效期到 2050 年，无需刷新 |
| `mqtt.c:284` | `esp_mqtt_client_publish(..., 0, 1, 0)` → **QoS 1** | ✅ 正好满足项目书"按序号检查缺失" |
| `.disable_auto_reconnect = true` | 已设 | ✅ 原作者已考虑 |

**必须改的只有两处**：

1. **删掉重连退避定时器**：`mqtt.c:13-18`（退避状态）、`mqtt.c:148-165`（回调）、`mqtt.c:205-225`（`MQTT_EVENT_DISCONNECTED` 里的指数退避）、`mqtt.c:310-318`（定时器创建）
2. **把 `onenet_start()` 从"非阻塞"改成"带超时等待 `MQTT_EVENT_CONNECTED`"**；并新增"发布后带超时等 `MQTT_EVENT_PUBLISHED`"（即等 PUBACK）

**C. 凭据**：把 `app_secrets.h` 复制到 `firmware\main\` 并填入新产品的凭据，同时提交一份 `app_secrets.h.example` 作为模板。

**D. OneJSON 组装**：`main\app_payload.c` 写纯函数，输入数值输出 OneJSON 字符串（可单测）。

**为什么**

- **为什么复用而不是新写**：`MY_MQTT` 已经把 OneNET 最难的部分（TLS + CA + HMAC token + 物模型主题 + 分片接收）都实现了。新写要重新踩一遍证书、签名、OneJSON 格式的坑。
- **为什么 QoS 必须是 1**：项目书 1.3 节要求"服务端按序号检查缺失"。QoS0 是"发出去就不管了"——TCP 缓冲区里的数据可能还没到 broker，我们就已经关掉 WiFi 去睡了，**这样的"成功"是假的**。QoS1 会等 PUBACK，收到才算成功，这个信号才能用来量化上报成功率。

**验证（对应项目书调试步骤 3）**

- 串口打印 `MQTT connected`，且 **TLS 握手没有证书报错**
- OneNET 控制台 → 设备 `node01` → 状态变"在线"（短暂），"设备数据流"里 9 个属性都有值
- `seq` 逐条递增
- 拔掉路由器网线 → 节点打印 WiFi 超时 → 恢复网线 → 下一轮自动恢复（步骤 8 的预演）

**坑**

- **物模型属性没先建好** → 平台静默丢弃，但节点看到 PUBACK 以为成功了。**这是最隐蔽的坑**，一定用控制台数据流逐字段核对
- `client_id` 必须唯一。和 AloT 共用凭据就会互相踢下线
- `MQTT_EVENT_PUBLISHED` 事件**只对 QoS1/QoS2 触发**，QoS0 不会触发。用错 QoS 会一直等不到事件
- **TLS 握手在 ESP32-S3 上要 1~3 秒、电流 100mA+**。项目书示例的超时是 4 秒，**不够**，本项目放宽到 8 秒
- OneNET 硬编码 CA 证书到期时间是 2049 年，短期无忧；但如果中国移动轮换 CA，代码里的证书会失效——报证书错误时先怀疑这个
- OneNET 免费额度/连接数有限制，24h 试运行前先确认够用

**这一课学到的**：选对平台能让工作量**下降一个数量级**；但平台也会把自己的数据模型强加给你，"发布成功"不等于"平台接受了"。

---

## 步骤 5：Wi-Fi 组件改造 + 全链路串通

**目标**：节点稳定地走完「采样 → Wi-Fi → TLS → OneNET 上报」。

**做什么**

复制 AloT `components\WIFI` → `firmware\components\wifi_sta`，三处改动：

1. `wifi_init_sta()` 增加超时参数，`wifi.c:127` 的 `portMAX_DELAY` → `pdMS_TO_TICKS(timeout_ms)`，**超时必须返回失败而不是永远等**
2. **删掉** `wifi_start_slow_retry()` 与 `xTimerCreate("wifi_retry", pdMS_TO_TICKS(10000), ...)`，以及 `WIFI_FAST_RETRY_MAX 5` 的快速重试循环
3. 增加 `wifi_stop()`：`esp_wifi_disconnect()` + `esp_wifi_stop()`，睡眠前调用，彻底关射频

然后把 `main.c` 串起来：连 WiFi → 连 MQTT → 发 OneJSON → 断开 → 等 30 秒 → 重复（调试期 30 秒，先不上深睡）。

**为什么**

- **为什么 `esp_wifi_stop()` 而不只是 `disconnect()`**：`disconnect` 只是断开连接，射频电路仍在工作，功耗依然高。`stop` 才真正关掉 WiFi 子系统。
- **为什么保留 PHY 校准数据**：首次连接要做 PHY 校准（1~3s、100mA+），IDF 默认把校准结果存进 NVS，**第二次起复用**（快得多）。**千万别关掉这个选项**，否则每次唤醒都要多花几秒。

**验证**

- 控制台持续收到 OneJSON，9 个字段齐全且数值合理
- `seq` 逐条递增
- 记下"唤醒→上报完成"的耗时，作为步骤 9 算续航的输入

**这一课学到的**：把"关掉"和"断电"区分清楚，是低功耗设计的基本功。

### 5.1 第一次实测（2026-10-11，`APP_DEBUG_STAGE = 4`）：链路全通，卡在鉴权

固件 803,856 字节，烧进 COM6 后连跑 5 轮，串口实录如下。

**已经打通的部分**

| 环节 | 实测结果 |
|---|---|
| Wi-Fi 连接 | SSID `602`，5 轮耗时 **1409 / 1829 / 1850 / 1999 / 2528 ms**，IP `192.168.1.102` |
| RSSI | **-27 ~ -29 dBm**（很近，信号极好） |
| TLS 握手 | **成功**，`tls_last_err=0x0 tls_stack_err=0 sock_errno=0` |
| MQTT 到达服务端 | **成功**，`studio-mqtts.heclouds.com:8883` 正常回了 CONNACK |
| token 生成 | 143 字符，与 PC 端算出的逐字一致 |
| DHT11 | 温度 24.1~25.3 ℃ / 湿度 68~98 %RH，校验通过 |
| 单轮总耗时 | **5928 ~ 6077 ms**（读 DHT11 + 连 Wi-Fi + TLS + MQTT 失败） |
| `publish_failures` | 逐轮 0 → 1 → 2 → 3 → 4 递增（送的是"截至发送这一刻"的值） |

**卡住的部分**：MQTT 服务端回 **`CONNACK=4`（bad username or password）**。
固件里 `onenet.c` 打的完整错误行是：

```
E (4749) onenet: MQTT 建连失败：type=2 tls_last_err=0x0 tls_stack_err=0 sock_errno=0 connect_return_code=4
W (4749) mqtt_client: Connection refused, bad username or password
```

注意 `tls_last_err=0x0` —— **TLS 层完全干净**，所以问题不在证书、不在 Wi-Fi、不在网络，
纯粹是「平台不认这套鉴权参数」。

**顺带观察到的三件事（都不是 bug，但值得记下）**

1. **上电头两次读到 98.0 / 97.0 %RH**，被 `APP_HUMI_MAX_RH = 95.0` 判为越界 →
   `sensor_ok = false`。这正是设计意图：**越界的读数不许上报成"有效"**，
   而且不许拿上一次的值冒充。第三次起降到 89 / 76 / 68 %RH，恢复正常。
2. 断开时串口出现 `W wifi_sta: AP 断开 reason=8 ... 1/3`。
   `reason=8 = WIFI_REASON_ASSOC_LEAVE` —— 这是**我们自己调 `esp_wifi_disconnect()`
   触发的正常现象**，不是掉线故障。日志里看到它不用慌。
3. IDF 报 `W wifi: Password length matches WPA2 standards, authmode threshold
   changes from OPEN to WPA2`。我们在 `wifi_sta.c` 里把 `threshold.authmode` 写成
   `WIFI_AUTH_OPEN`（比 AloT 写死 `WPA2_PSK` 更宽容），IDF 看到密码长度够 8 位
   会自动把阈值上调到 WPA2 —— **两种写法在这个网络下都能连上**，
   但我们的写法在 WPA/WPA2 混合或 WPA3-only 的路由器上不会出现"密码明明对却连不上"。

**鉴权排查（44 个变体，全部失败）**

PC 端写了三个探针脚本（都只用标准库，不需要板子），把所有能想到的维度扫了一遍：

| 脚本 | 扫了什么 | 变体数 | 结果 |
|---|---|---|---|
| `tools\onenet_auth_probe.py` | 签名算法 sha256/sha1/md5、密钥是否 base64 解码、新旧接入点、et 取值 | 8 | 全 4 |
| `tools\onenet_auth_matrix.py` | 不存在的产品ID / 不存在的设备名 / 乱码密码 / username 与 client_id 对调 / 空 username | 9 | **全 4** |
| `tools\onenet_auth_forms.py` | et 取 now+1h/1d/30d/1y、`res` 与 `sign` 编码与不编码的四种组合 | 11 | 全 4 |
| `tools\onenet_auth_endpoints.py` | 4 个可达接入点（`studio-mqtts:8883`、`studio-mqtt:1883`、`mqtts:8883`、`mqtt:6002`）× 4 种 password 形态（密钥原文 / 解码明文 / 两种 token） | 16 | 全 4 |

**最关键的一条结论**：`onenet_auth_matrix.py` 证明平台在这一点上是**黑盒** ——
连"产品ID 和设备名都故意填成不存在的值"也是回 4。所以**返回码无法区分
"签名错"与"资源不存在"**，继续在 PC 端穷举已经没有信息量了。

剩下的唯一可能：**控制台里的产品ID / 设备名称 / 设备密钥三个值与固件里写的不一致**。
排查顺序与「为什么必须先跳过 CN 校验」记在 `docs\ONENET_SETUP.md` §6.1。

---

## 步骤 6：深度睡眠闭环 —— 架构形态的切换点

**目标**：节点进入"每 N 分钟执行一次"的形态。

**做什么**

1. `RTC_DATA_ATTR` 保存跨睡眠变量：

   ```c
   RTC_DATA_ATTR uint32_t sequence_number;
   RTC_DATA_ATTR uint32_t publish_failures;
   RTC_DATA_ATTR uint32_t sensor_fail_count;
   RTC_DATA_ATTR uint32_t co2_tick;          // CO2 降频计数
   RTC_DATA_ATTR uint16_t co2_baseline, tvoc_baseline;   // SGP30 基线
   ```

2. 开机第一件事打印唤醒原因：

   ```c
   esp_sleep_wakeup_cause_t r = esp_sleep_get_wakeup_cause();
   // 0 = ESP_SLEEP_WAKEUP_UNDEFINED（上电/复位）
   // 4 = ESP_SLEEP_WAKEUP_TIMER（定时器唤醒）
   ```

3. **所有分支都必须汇聚到同一个 `enter_deep_sleep()`**——传感器失败、WiFi 失败、MQTT 失败、任何异常，一条都不能漏
4. **关闭板载 RGB 灯**：先按 v1.1 的 **GPIO38** 写，设成全黑；然后**实际观察灯有没有灭**来验证引脚是否正确
5. 用 `esp_timer_get_time()` 在流程首尾打点，记录"唤醒→睡下"的实际耗时
6. 测试期周期设 30 秒

**CO₂ 降频逻辑**：

```c
co2_tick++;
bool do_co2 = (co2_tick >= CO2_EVERY_N_WAKES);   // N = 3，即 30 分钟
if (do_co2) co2_tick = 0;
```

**为什么**

这是整个项目的**分水岭**。代码通过这一步从"持续运行的系统"变成"周期性执行的脚本"。一旦接受这个形态，很多设计就顺理成章了：为什么不用看门狗、为什么不用 RTOS 任务、为什么每次都要重新初始化——因为每次运行都是全新的开始。

**验证（对应项目书调试步骤 4）**

- `idf.py monitor` 里**每 30 秒出现一次完整的启动日志**
- 两次日志之间**串口完全没有输出** → 这才是"真的睡了"的铁证
- `seq` 每次 +1 → RTC 内存生效
- `wake_reason` 每次都是 **4**（定时器），不是 0（异常复位）
- **板载 RGB 灯在深睡时熄灭** → 同时验证了 GPIO38 这个引脚是对的
- `co2_tick` 按 1→2→3→0 循环 → CO₂ 每 3 个周期采一次

**坑**

- `esp_deep_sleep_start()` **不会返回**。写在它后面的代码是死代码，编译可能报警告，这是正常的
- 深睡会让 USB 重新枚举，`idf.py monitor` 有时会掉线。Ctrl+] 退出重进即可
- **最隐蔽的坑**：某条失败分支忘了调 `enter_deep_sleep()`，直接 `return` → `app_main` 返回后 FreeRTOS 会走 `esp_restart()` → 芯片重启而不是睡觉 → 变成**高频重启**，比不睡还费电。这个 bug 在串口日志里表现得很像"正常工作"，只有看电流或看 `wake_reason=0` 才能发现
- 如果按 GPIO38 写灯没灭，说明映射不对（可能是 GPIO48），**这正是我要实测的原因**

**这一课学到的**："所有失败路径都必须收敛到同一个出口"是低功耗系统的核心不变量，必须有意识地设计。

---

## 步骤 7：传感器电源门控 + 休眠电流实测

**目标**：深睡时 DHT11 的 VCC 实测接近 0V；并**确定 SGP30 的最优供电方案**。

**A. DHT11 门控**（读完传感器后）：

```c
// 1. 数据线设为输入，切断内部上拉
gpio_set_direction(DATA_PIN, GPIO_MODE_INPUT);
gpio_set_pull_mode(DATA_PIN, GPIO_FLOATING);

// 2. 传感器电源拉低
gpio_set_level(POWER_PIN, 0);

// 3. 让引脚在深睡期间保持这个电平（否则深睡时 GPIO 变高阻，传感器又被"半供电"）
gpio_hold_en(POWER_PIN);
gpio_deep_sleep_hold_en();
```

唤醒时先解除保持，再上电：

```c
gpio_deep_sleep_hold_dis();
gpio_hold_dis(POWER_PIN);
gpio_set_level(POWER_PIN, 1);
```

**为什么**：

- DHT11 待机约 **40~50µA**。听起来极小，但第二阶段目标是 **<100µA**——这 50µA 就吃掉一半预算
- 更关键的是 **DATA 线的倒灌路径**：如果内部上拉还开着，电流会从 3V3 经上拉电阻到 DATA，再通过 DHT11 内部的保护二极管灌进它的 VCC 脚，**等于给一个"已断电"的传感器半供电**
- **深睡时 GPIO 默认变高阻态**，所以光 `gpio_set_level(0)` 不够，必须用 `gpio_hold` 把电平钉住

**B. SGP30 供电方案裁决**（用步骤 3 实测的数字）：

| 实测结果 | 决策 |
|---|---|
| 静态电流 ≤ 100µA | **方案 A：常供 3.3V**。免掉 15 秒预热，也免掉 MOSFET |
| 静态电流 > 1mA | **方案 B：GPIO + P 沟道 MOSFET 门控**，接受每次 15 秒预热（藏进 WiFi 连接时间里） |
| 50µA ~ 1mA | 算总账：`静态电流 vs 每次 0.2mAh 预热的等效平均电流`，取小者。**这里需要你拍板**（触发停止条件 3） |

**C. 实测休眠电流**：

- 3V3 排针串电流表，或用 USB 电流表
- 分别在**开发板 USB 供电**和**电池供电**两种情况下测
- 记录：唤醒期间的峰值、深睡期间的稳定值

**D. 拆解电流去向**：电源指示灯（硬件直连，关不掉）、USB-UART 桥接芯片、LDO 静态电流、RGB 灯、SGP30 静态电流。

**验证（对应项目书调试步骤 5）**

万用表**直流电压档**，红表笔接 GPIO5、黑表笔接 GND：

| 时刻 | 期望读数 |
|---|---|
| 唤醒后 / 读取时 | 约 3.3V |
| **深睡中** | **接近 0V（理想 <0.1V）** |

同时确认：唤醒后读数仍然稳定正常 → 说明反复断电上电没有损伤传感器。

**坑**

- 如果测到 **>0.3V**，说明有漏电流路径。这触发 `PLAN.md` 的**停止条件 3**
- **3 针模块有个隐藏优势**：它自带的 10k 上拉接在**模块自己的 VCC 和 DATA 之间**。因为 VCC 由 GPIO5 供电，GPIO5 拉低时上拉另一端也变 0V，**不会倒灌**。而 4 针裸件如果外接上拉到不受控的 3V3，就一定会倒灌。这是选 3 针模块的额外好处
- `gpio_hold` 的开关必须成对，忘了 `gpio_deep_sleep_hold_dis()` 会导致下次唤醒引脚状态异常
- **SGP30 用电解电容/大电容时**，门控关断后电容会继续给传感器供电一会儿，测 VCC 时要等几秒

**这一课学到的**：低功耗优化的收益往往不在"主芯片"，而在那些你根本没注意的旁路路径上。**而"常供电"有时反而比"门控"更省——因为省掉的是预热。**

---

## 步骤 8：鲁棒性测试 —— 注入四类故障

**目标**：证明节点在任何故障下都不会卡死。

| 故障 | 怎么造 | 期望行为 |
|---|---|---|
| **断 WiFi** | 关掉路由器 / 让节点连不上 | 10 秒内放弃 → 睡眠；恢复后自动恢复上报 |
| **断公网** | 路由器 WAN 口断，但局域网正常 | TLS 握手失败 → 8 秒超时 → 睡眠；恢复后自动恢复 |
| **拔传感器 DATA 线** | 拔掉 GPIO4 那根 | 上报 `sensor_ok:false`，**不是卡死，也不是拿上次的值糊弄** |
| **SGP30 无响应** | 拔掉 SGP30 的 SDA 或 VCC | CO₂/TVOC 字段缺省或标失败，**DHT11 与上报不受影响**，正常睡眠 |

**为什么**

对应项目书 5.4 节的失败处理，也对应 1.3 节的验收项"断网恢复后无需人工复位"。

在常供电系统里这类测试只问"能不能恢复"；在电池系统里问的是"**能不能不把电池烧光**"。同一件事，严重性完全不同。

**验证（对应项目书调试步骤 7）**

- 每种故障下，节点都在**预期时间内回到睡眠**（看串口静默的时机）
- 故障恢复后，**无需任何人工干预**，下一周期自动正常上报
- 日志里失败计数在故障期间递增，恢复后停止增长
- `sensor_ok` 正确反映传感器状态，绝不"用旧值伪装"
- **SGP30 故障不能拖累 DHT11 链路**——这是新增的验收点

**坑**

- 拔 DATA 线这种"故障"在软件里看起来和"传感器坏了"一样，要确认代码走的是 `sensor_ok=false` 分支而不是"沿用上次成功值"
- I2C 总线卡死是经典问题：某个从设备把 SDA 拉低不放，整条总线瘫痪。要让 SGP30 的失败路径**只影响 SGP30**
- 改路由器设置（断 WAN）会同时影响你的其他设备，注意通知同网人

**这一课学到的**：低功耗设备的故障处理，本质上是"功耗预算的防守"；而且**每个外设都要有独立的失败隔离**。

---

## 步骤 9：24 小时试运行与验收统计

**目标**：用数据证明达到了项目书 1.3 节的指标。

**做什么**

1. 周期从 30 秒改回 **600 秒**（10 分钟）
2. 在 OneNET 控制台创建**应用/项目**并生成 API 密钥（用于拉数据）
3. 写 `tools\onenet_pull.py`：定时调用 OneNET **应用 API「设备属性记录查询」**，把历史数据落成本地 CSV
   - 文档：<https://open.iot.10086.cn/doc/iot_platform/book/api/application/queryDevicePropertyHistory.html>
   - 备用路径：OneNET **规则引擎 → 消息队列MQ / HTTP推送**
4. 写 `tools\analyze_run.py`（**只用 Python 标准库**）统计：

   | 统计项 | 算法 | 验收线 |
   |---|---|---|
   | 上报成功率 | 实收条数 ÷ 理论条数（24h ÷ 10min = 144） | **≥98%** |
   | 采样成功率 | `sensor_ok=true` 的比例 | **≥99%** |
   | CO₂ 采样率 | 实收 CO₂ 条数 ÷ 理论条数（24h ÷ 30min = 48） | 参考（新增） |
   | 丢失定位 | `seq` 序列里缺哪些号 | 能列出具体编号 |
   | 周期精度 | 相邻消息时间间隔的均值与标准差 | **±10s** |
   | 复位情况 | `wake_reason` 分布是否全为 4 | 全为 4 |
   | 信号质量 | `rssi` 分布 | 参考 |

5. 生成验收报告，填项目书表 14

**为什么**

项目书的验收指标**全都是统计量**，没有一条是"看一眼对不对"。必须靠脚本算，靠人眼数 144 条不现实，而且无法复盘。

**坑**

- PC 睡眠、Windows 自动更新重启、防火墙弹窗 —— 任何一个都会污染数据。跑之前先关电源计划睡眠、推迟更新
- **平台时间戳的可靠性变了**：原来时间戳由本机 Mosquitto 订阅脚本打（你完全可控）；现在由 OneNET 平台打，所以**周期精度的测量依赖平台记录的准确性**。如果发现周期抖动异常，先用 `onenet_pull.py` 里各自的本地接收时间做交叉验证
- 节点时间戳靠平台，所以 **PC 自身时钟也要准**。先跑 `w32tm /resync`
- OneNET 数据存储期有限制，24h 数据要及时拉取，别等一周后再去查

**这一课学到的**：验收指标必须在项目开始时就设计成"可被自动计算"的形式；换成云平台后，"谁打时间戳"变成一个必须重新确认的问题。

---

## 步骤 10：第二阶段 —— 电池与休眠电流

**目标**：脱离 USB，用电池连续工作 7 天以上。

**做什么**

1. **先量开发板自身的休眠电流**（步骤 7 已测）
2. **拆解电流去向**：电源指示灯（硬件直连，软件关不掉）、USB-UART 桥接芯片、LDO 静态电流、RGB 灯、SGP30 静态电流
3. 方案 A（不改硬件）：对照 **<5mA** 指标
4. 方案 B（硬件改造）：
   - 换低静态电流 LDO（如 TPS7A02，静态 1µA 级）
   - 去掉/断开电源指示灯
   - 只保留必要外设
   - 目标 **<100µA**（理想 20~50µA）
5. 接入 18650 / LiPo + 充电保护板

**续航估算（已按本方案修订）**

单次唤醒工作耗电（含 TLS，按 120mA × 6s 估）：

```
120mA × 6s = 720 mAs = 0.2 mAh / 次
```

SGP30（每 30 分钟一次）：`48.8mA × 15s = 0.203 mAh / 次`

| 场景 | 平均电流 | 续航（3000mAh × 70% = 2100mAh） |
|---|---|---|
| 项目书原估算（无 CO₂、无 TLS、硬件优化） | 0.82 mA | ≈ **107 天** |
| 本方案 + 硬件优化（有 TLS，无 CO₂） | 1.22 + 0.02 = **1.24 mA** | ≈ **70 天** |
| **本方案 + 硬件优化 + SGP30 每 30 分钟** | 1.24 + 0.41 = **1.65 mA** | ≈ **53 天** |
| 本方案 + **开发板未改硬件** + SGP30 每 30 分钟 | 1.22 + 2 + 0.41 = **3.63 mA** | ≈ **24 天** |

> 这些是**上界估算**，实际以步骤 7/9 的实测为准。TLS 握手让单次唤醒从项目书假设的 4 秒变成约 6 秒，这是本方案相对项目书估算的主要退化来源。

**关键洞察**：最容易省电的一招不是抠 µA，是**把周期拉长**。CO₂ 从 10 分钟改到 30 分钟一次，就把平均电流从 2.44mA 降到 1.65mA（**省 32%**）。项目书 4.1 节说的"把周期从 1 分钟改到 10 分钟，比所有软件微优化加起来都有效"，就是这个意思。

**踩坑提示**：在开发板上无论怎么优化软件，都不可能突破"电源指示灯 + USB 芯片 + LDO"这个硬件地板。**想做 <100µA，必须动硬件**，这是第二阶段的分界线。

---

## 附：GitHub 仓库对接

**仓库现状（已核实）**

| 项 | 值 |
|---|---|
| 地址 | `https://github.com/lyl20250610/ESP32-S3-Low-power-environmental-sensor.git` |
| 可见性 | **公开（public）** ⚠️ |
| 默认分支 | `main` |
| 内容 | **只有一个 93 字节的 `README.md`**（提交 `4bac871`） |
| 创建时间 | 2026-10-08 |

基本是个空仓库，可以自由规划结构。

**本机 git 现状**

- 全局 `user.name` / `user.email`：**未配置**
- `credential.helper`：**未配置**
- scoop 版 git **自带** `C:\Users\Lenovo\scoop\apps\git\current\mingw64\bin\git-credential-manager.exe`（不在 PATH 上）
- Windows 凭据管理器里**没有**已保存的 GitHub 凭据
- **访问 GitHub 不稳定**（实测 `curl 28 Recv failure: Connection was reset`）

→ 推送前需先解决认证（配好 Git Credential Manager，或用 Personal Access Token）。步骤 1 处理。

**建议的仓库结构**

整个 `D:\esp32\xiangmu1` 作为仓库根，因为 `docs/`、`tools/` 都是项目的一部分：

```
xiangmu1/
├─ README.md              进度看板
├─ .gitignore             ★ 第一件事：排除 app_secrets.h、firmware/build/
├─ docs/                  PLAN.md、WALKTHROUGH.md、HARDWARE.md
├─ firmware/              ESP-IDF 工程
│  ├─ main/               main.c、app_config.h、app_secrets.h(.example)、app_payload.c/.h
│  ├─ components/{dht11,wifi_sta,onenet,sgp30}/
│  ├─ CMakeLists.txt
│  └─ sdkconfig.defaults
└─ tools/                 dump_docx.py、onenet_pull.py、analyze_run.py
```

⚠️ `firmware/build/` 是几万个小文件的构建产物，**必须 `.gitignore` 排除**，否则仓库会被撑爆。
⚠️ `app_secrets.h` 含 WiFi 密码和 OneNET 密钥，**必须排除**（项目书 1.1 节明确要求）。本仓库是**公开**的，误提交 = 真泄露。
