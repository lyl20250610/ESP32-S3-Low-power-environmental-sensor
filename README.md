# ESP32-S3 超低功耗无线环境监测节点

一款基于 ESP32 的远程低功耗环境检测器：**定时唤醒 → 采样温湿度/CO₂ → Wi-Fi → OneNET 上报 → 深度睡眠**。

依据文档：`ESP32-S3超低功耗无线环境监测节点项目搭建方案.docx`（版本 1.1）

**文档导航**

| 文档 | 内容 |
|---|---|
| [`docs/PLAN.md`](docs/PLAN.md) | 实施计划：架构、逐步方案、风险、停止条件、核查表 |
| [`docs/WALKTHROUGH.md`](docs/WALKTHROUGH.md) | 逐步讲解：每一步的"做什么 / 为什么 / 怎么验证 / 坑" |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | 软件架构：分层、配置边界、一次唤醒的流程、六条不变量 |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | 接线确认单：引脚分配、必须回避的脚、实测记录 |
| [`docs/ONENET_SETUP.md`](docs/ONENET_SETUP.md) | ★ OneNET 控制台配置：建产品 / 设备 / **9 个物模型属性** |

---

## 硬件

| 项 | 型号 |
|---|---|
| 开发板 | ESP32-S3-DevKitC-1 **v1.1** |
| 模组 | ESP32-S3-WROOM-1-**N8R8**（8MB Flash + 8MB Octal PSRAM，本项目关闭 PSRAM） |
| 温湿度 | DHT11（3 针带小板模块，自带上拉） |
| 空气质量 | SGP30（eCO₂ + TVOC，I²C） |
| 平台 | 中国移动 OneNET Studio（MQTT over TLS, 8883） |

## 目录结构

```
xiangmu1/
├─ firmware/                  ESP-IDF 工程（框架 v5.4.3）
│  ├─ CMakeLists.txt
│  ├─ sdkconfig.defaults      8MB Flash、PSRAM 关闭
│  ├─ partitions.csv
│  └─ main/
│     ├─ main.c               一次唤醒周期的业务编排
│     ├─ app_config.h         引脚/周期/超时集中配置
│     └─ app_secrets.h.example  凭据模板（真实文件 app_secrets.h 不入库）
├─ docs/
│  ├─ PLAN.md                 实施计划（含偏离说明与核查表）
│  ├─ WALKTHROUGH.md          逐步讲解
│  ├─ ARCHITECTURE.md         软件架构（分层 / 配置分工 / 唤醒流程 / 不变量）
│  ├─ HARDWARE.md             接线确认单
│  └─ ONENET_SETUP.md         OneNET 控制台配置（含 9 个物模型属性）
└─ tools/
   ├─ idf.ps1                 ★ 本机专用的 IDF 环境激活脚本
   ├─ idf.cmd                 ★ .ps1 被策略拦截时的包装入口
   ├─ dump_docx.py            项目书导出为纯文本
   ├─ onenet_pull.py          OneNET 历史数据拉取（待实现）
   └─ analyze_run.py          24h 验收统计（待实现）
```

---

## 快速开始（本机特有注意事项）

### ⚠️ 不要直接跑 ESP-IDF 的 `export.ps1`

本机 PATH 上的 `python` 是 **Microsoft Store 的占位程序**，会让 `export.ps1`
在执行 `$idf_exports = python ... activate.py --export` 时拿到空值并抛 `BadExpression`，
结果是 PATH 里根本没有 `idf.py`。**请一律用本工程的 `tools/idf.ps1`**：

```powershell
cd D:\esp32\xiangmu1

.\tools\idf.ps1 build                    # 编译
.\tools\idf.ps1 -p COM4 flash            # 烧录（COM 口改成你实际的）
.\tools\idf.ps1 -p COM4 monitor          # 看串口日志（Ctrl+] 退出）
.\tools\idf.ps1 -p COM4 flash monitor    # 烧完直接看日志
.\tools\idf.ps1 fullclean                # 彻底清理构建产物
```

**如果 PowerShell 报"未对文件进行数字签名，无法在当前系统上运行该脚本"**，
说明本机执行策略拦了脚本。两种解法（任选）：

```powershell
.\tools\idf.cmd build                          # 解法一：用 .cmd 包装脚本（推荐）
powershell -ExecutionPolicy Bypass -File .\tools\idf.ps1 build    # 解法二：显式放行
```

> 本机已实测到的三个环境坑，都已在 `tools/idf.ps1` 里绕开，详见脚本头部注释：
> ① PATH 上的 `python` 是 Microsoft Store 占位程序，会让官方 `export.ps1` 失效；
> ② `param()` 会把 `build` 抢去当位置参数、把 `-p` 当未知参数报错；
> ③ PowerShell 5.1 读「无 BOM 的 UTF-8」`.ps1` 会按 GBK 解码 → 中文注释变乱码、语法错误。

### 填凭据（联网步骤之前必做）

```powershell
Copy-Item firmware\main\app_secrets.h.example firmware\main\app_secrets.h
# 然后编辑 firmware\main\app_secrets.h，填入 Wi-Fi 与 OneNET 三元组
```

> 🔒 **本仓库是 PUBLIC 的。** `app_secrets.h` 已被 `.gitignore` 排除，
> 提交前请务必 `git status` 复核，确认它不在待提交列表里。

### 提交与推送（本机必须走代理，否则连不上 GitHub）

**症状**：`git fetch` / `git push` 报
`Failed to connect to github.com port 443 after 21000 ms: Could not connect to server`，
或 `Recv failure: Connection was reset`。
**但浏览器和 `Invoke-WebRequest https://github.com` 却是 200** —— 这个组合就是本机代理的典型特征。

**根因**：本机系统代理（MyClash）在 `127.0.0.1:7877`。
浏览器、VS Code、PowerShell 的 `Invoke-WebRequest` 都会自动读取 WinINET 代理设置，
而 **`git.exe` 和 `curl.exe` 不会**，所以它们直连 `20.205.243.166:443` 就会超时。

**已在本仓库配好**（`.git/config`，只对本仓库生效）：

```powershell
git config http.proxy  http://127.0.0.1:7877
git config https.proxy http://127.0.0.1:7877
```

想对所有仓库生效就把 `--global` 加上；想取消就 `git config --unset http.proxy`。
**注意**：代理关掉时 git 会连不上，届时记得取消这两项。

**诊断命令**（下次再遇到类似"网页能开、命令行连不上"，先查这两条）：

```powershell
Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' |
    Select-Object ProxyEnable, ProxyServer
Get-Process | Where-Object ProcessName -match 'clash|v2ray|xray|sing-box'
```

### ⚠️ VS Code 里满屏红线（"无法打开源文件 xxx.h"）

**这不是编译错误，是编辑器的 IntelliSense 找不到头文件。** 判断依据很简单：
只要 `.\tools\idf.ps1 build` 能正常退出码 0，代码本身就是好的，红线只说明
C/C++ 插件没拿到 ESP-IDF 的包含路径。

**根因**：VS Code 的 C/C++ 插件会给新工程**自动生成一份通用 Win32/MinGW 模板**——
`firmware\.vscode\c_cpp_properties.json` 里 `includePath` 只有 `${workspaceFolder}/**`，
`compilerPath` 指向本机 MinGW 的 `gcc.exe`，`defines` 是 `_DEBUG/UNICODE`。
这些跟 ESP-IDF / Xtensa 毫无关系，所以 `freertos/FreeRTOS.h`、`esp_log.h`、
`driver/gpio.h` 一个都找不到。

**修法**：让插件直接读 ESP-IDF 构建时产出的编译数据库 `firmware\build\compile_commands.json`
（`idf.py build` 每次都会生成，里面每个源文件都带完整的 71 条 `-I` 参数），
并把 `compilerPath` 指向 Xtensa 工具链（用于 `<stdio.h>` `<string.h>` 这类编译器自带头文件）。
本工程的 `firmware\.vscode\c_cpp_properties.json` **已经这样配好了**。

改完红线若仍在，执行一次 **`Ctrl+Shift+P` → `C/C++: Reset IntelliSense Database`**，
或直接 **Developer: Reload Window** 重载窗口。

> **为什么要靠 `compile_commands.json`**：ESP-IDF 的包含路径有 70 多条，且随 IDF 版本变化；
> 手写进 `includePath` 既容易漏也必然过期。编译数据库是**构建系统自己产出的唯一真值**。
> 注意 `firmware\build\` 被 `.gitignore` 排除，所以新克隆的仓库**要先 build 一次**才会有这个文件。
>
> 本配置假设你用 **`firmware` 目录**作为 VS Code 工作区（推荐）。若改成打开仓库根目录，
> 请把 `compileCommands` 改成 `"${workspaceFolder}/firmware/build/compile_commands.json"`。

---

## 进度看板

| 步骤 | 内容 | 状态 |
|---|---|---|
| 0 | 硬件/接线/OneNET 账号确认 | 🟡 板子 N8R8 / COM6 已实测；OneNET 已建好，待核对 9 个属性与凭据 |
| 1 | 工程骨架 + N8R8 配置 + 编译烧录自检 | ✅ 编译零警告、烧录成功、串口自检日志完整 |
| 2 | 移植改造 DHT11（删轮询任务、加电源门控） | ✅ **验收通过**：连续 14 次 CRC 通过，23.4℃/38.0%RH；遗留"模块无上拉电阻"待办 |
| 3 | 移植改造 SGP30 + 实测预热时间与待机电流 | ⬜ |
| 4 | OneNET 建产品/设备/物模型 9 属性 + 改造 onenet 组件 | ⬜ |
| 5 | 改造 wifi_sta 加硬超时 + 串通全链路 | ⬜ |
| 6 | 深度睡眠闭环 + 关板载 RGB 灯 | ⬜ |
| 7 | 传感器电源门控 + 实测休眠电流与 VCC | ⬜ ⚠️ 开工前须先解决 DHT11 上拉电阻（否则冷启动读不到数） |
| 8 | 四类故障注入测试 | ⬜ |
| 9 | 24h 试运行 + 验收统计 | ⬜ |
| 10 | 第二阶段：电池供电与休眠电流优化 | ⬜ |

> **遗留待办**：DHT11 模块上**没有 4.7k~10k 上拉电阻**（步骤 2 自检确证），目前靠 ESP32 内部约 45 kΩ 工作。
> 标准修法 = DATA↔3V3 间加一颗 4.7 kΩ。用户决定暂不加、继续推进；**步骤 7 之前必须解决**。详见 [`docs/HARDWARE.md`](docs/HARDWARE.md) §2.2。

> 详细的分步做法、每一步的"为什么"和"坑"，见 [`docs/WALKTHROUGH.md`](docs/WALKTHROUGH.md)。
