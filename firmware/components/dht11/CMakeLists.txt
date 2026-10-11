# DHT11 单总线温湿度驱动（低功耗改写版）
#
# REQUIRES 说明：
#   driver   —— gpio_config / gpio_set_level / gpio_set_direction
#   esp_timer—— esp_timer_get_time()，微秒级时序测量
#   freertos —— vTaskDelay（公共依赖，显式写出便于阅读）
idf_component_register(
    SRCS "dht11.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer freertos
)
