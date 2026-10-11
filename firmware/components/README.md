# firmware/components/ —— 本工程自有的 ESP-IDF 组件

ESP-IDF 会自动扫描这个目录下的每个子文件夹，把含 `CMakeLists.txt`
且调用了 `idf_component_register()` 的文件夹当成一个组件。

本目录目前是**空的**（步骤 1 只搭骨架）。后续步骤会陆续放进来：

| 组件 | 来源 | 放在哪一步 |
|---|---|---|
| `dht11/` | 改自 `D:\esp32\ESP32-S31\ESP32-S3\AloT\components\DHT11\` | 步骤 2 |
| `sgp30/` | 改自 AloT 的 `SGP30`（换新版 I2C 驱动） | 步骤 3 |
| `onenet/` | 改自 AloT 的 `MY_MQTT`（去掉重连退避定时器） | 步骤 4 |
| `wifi_sta/` | 改自 AloT 的 `WIFI`（去掉 `portMAX_DELAY` 与 10s 重连定时器） | 步骤 5 |

> 之所以保留一个空的 `components/`：顶层 `CMakeLists.txt` 里如果
> `set(EXTRA_COMPONENT_DIRS ".../components")` 指向不存在的目录，
> ESP-IDF 会在 `tools/cmake/project.cmake:472` 直接报
> `Directory specified in EXTRA_COMPONENT_DIRS doesn't exist` 并中止编译。
