# OneNET MQTT 上报组件（步骤 4）
#
# REQUIRES 说明：
#   mqtt       esp-mqtt 客户端（CONFIG_MQTT_TRANSPORT_SSL=y 已开，支持 8883）
#   mbedtls    算 token 的 HMAC / Base64；同时是 TLS 的提供者
#   esp_timer  计时与"等 PUBACK 多久"的耗时统计
#   esp_event  MQTT 事件回调注册
# freertos 由 ESP-IDF 隐式提供（freertos 属于公共依赖，无需列出）。

idf_component_register(
    SRCS "onenet.c" "onenet_token.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt mbedtls esp_timer esp_event
)
