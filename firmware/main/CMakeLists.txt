# main 组件：一次唤醒周期的业务编排
#
# 【一个容易踩的坑】idf_component_register 的 REQUIRES 一旦写了内容，
# 就不再享有"没写 REQUIRES 时自动获得全部组件"的旧行为，而是**只**能看见
# 自己声明的依赖 + ESP-IDF 的公共依赖（esp_common / freertos / log / heap /
# esp_system / newlib / soc / hal …）。所以下面必须把用到的头文件的提供方
# 逐个列出来，否则会报 `fatal error: xxx.h: No such file or directory`，
# 并附带一句 "add <component> to PRIV_REQUIRES list of idf_component_register"。
# 步骤 1 之所以能编过，正是因为那时 REQUIRES 是空的。
#
# 依赖清单（随步骤推进继续追加）：
#   dht11          —— 步骤 2 起的温湿度驱动（它自己又带了 driver / esp_timer）
#   esp_hw_support —— esp_chip_info.h / esp_sleep.h
#   esp_app_format —— esp_app_desc.h
#   spi_flash      —— esp_flash.h（读 Flash 容量）
#   （后续）步骤 3  sgp30；步骤 4  onenet + mqtt + json；步骤 5  wifi_sta + esp_wifi
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES     dht11
    PRIV_REQUIRES esp_hw_support esp_app_format spi_flash
)
