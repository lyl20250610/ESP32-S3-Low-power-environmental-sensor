# wifi_sta 组件：为电池节点改造过的 Wi-Fi STA 连接
#
# 【坑 6 提醒】REQUIRES 一旦写了内容，就只剩自己声明的依赖 + ESP-IDF 公共依赖，
# 所以用到谁的头文件就必须列谁：
#   esp_wifi   —— esp_wifi.h（含 esp_wifi_sta_get_ap_info）
#   esp_netif  —— esp_netif.h / esp_netif_get_handle_from_ifkey
#   esp_event  —— esp_event.h（事件循环与事件注册）
#   nvs_flash  —— nvs_flash.h（Wi-Fi 驱动必须拿到 NVS 才能启动）
#   esp_timer  —— esp_timer.h（量连接耗时）
idf_component_register(
    SRCS "wifi_sta.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer
)
