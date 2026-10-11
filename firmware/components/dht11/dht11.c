#include "dht11.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "portmacro.h"

static const char *TAG = "dht11";


#define DHT11_TEMP_SANE_MIN_C   (-20.0f)
#define DHT11_TEMP_SANE_MAX_C   (60.0f)
#define DHT11_HUMI_SANE_MIN_RH  (0.0f)
#define DHT11_HUMI_SANE_MAX_RH  (100.0f)

// 传感器上电后，等待稳定的时间（ms）
static inline void delay_us(uint32_t us)
{
    const uint64_t start = (uint64_t)esp_timer_get_time();
    while (((uint64_t)esp_timer_get_time() - start) < (uint64_t)us) {
    }
}
//GPIO
static esp_err_t dht11_apply_read_pin(gpio_num_t data_pin)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << (uint32_t)data_pin,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

static esp_err_t dht11_power_on(gpio_num_t power_pin)
{
    if (power_pin < 0) {
        /* 传感器接的是常供 3.3V，没什么可做的 */
        return ESP_OK;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << (uint32_t)power_pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(power_pin, 1);
}

void dht11_release_data_pin(gpio_num_t data_pin)
{
    if (data_pin < 0) {
        return;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << (uint32_t)data_pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   /* ← 关键，详见 dht11.h 的说明 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

void dht11_safe_off(gpio_num_t data_pin, gpio_num_t power_pin)
{
    dht11_release_data_pin(data_pin);
    if (power_pin >= 0) {
        gpio_set_level(power_pin, 0);
    }
}


 //位时序


//读一个数据位：先等 50µs 的低电平过去，再量高电平的宽度
static uint8_t dht11_read_bit(gpio_num_t data_pin)
{
    uint64_t start = (uint64_t)esp_timer_get_time();
    while (gpio_get_level(data_pin) == 0) {
        if (((uint64_t)esp_timer_get_time() - start) > DHT11_BIT_TIMEOUT_US) {
            return 0;
        }
    }

    start = (uint64_t)esp_timer_get_time();
    while (gpio_get_level(data_pin) == 1) {
        if (((uint64_t)esp_timer_get_time() - start) > DHT11_BIT_TIMEOUT_US) {
            return 0;
        }
    }

    const uint64_t high_us = (uint64_t)esp_timer_get_time() - start;
    return (high_us > DHT11_BIT_ONE_MIN_US) ? 1 : 0;
}

/**
 * @brief 一次总线级读取（不含上电/断电）。调用前数据线必须已配好。
 * @note  函数内部会关中断约 4.7ms，但**保证**在返回前重新打开。
 */
static esp_err_t dht11_read_raw(gpio_num_t data_pin, float *temperature, float *humidity)
{
    uint8_t     data[5] = {0};
    esp_err_t   ret     = ESP_OK;
    const char *reason  = NULL;

    //起始信号：拉低 ≥18ms 
    gpio_set_direction(data_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(data_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(DHT11_START_LOW_MS));

    // 进入时序敏感区
    portDISABLE_INTERRUPTS();

    gpio_set_level(data_pin, 1);
    delay_us(30);                                /* 20~40µs 都可以 */
    gpio_set_direction(data_pin, GPIO_MODE_INPUT);   /* 释放总线，交给传感器 */

    // ---- 3. 等传感器的应答：80µs 低 → 80µs 高 → 第 1 位的 50µs 低 ----
    // 三个阶段分开报告，因为"病因"完全不同：
    //   阶段 1 超时（总线一直高）→ 传感器根本没响应（没接 / 没供电 / DATA 接错脚）
    //   阶段 2 超时（总线一直低）→ 总线被按住不放（短路 / DATA 与 GND 接反 / 传感器坏）
    //   阶段 3 超时（总线又变高）→ 应答之后的数据起始位没来（时序被打断）
    // 把这三条分清楚，调试时就不用靠猜。
    uint64_t start = (uint64_t)esp_timer_get_time();
    while (gpio_get_level(data_pin) == 1) {
        if (((uint64_t)esp_timer_get_time() - start) > DHT11_BIT_TIMEOUT_US) {
            ret    = ESP_ERR_TIMEOUT;
            reason = "传感器没有把总线拉低（没接 / 没供电 / DATA 接错脚）";
            goto done;
        }
    }
    start = (uint64_t)esp_timer_get_time();
    while (gpio_get_level(data_pin) == 0) {
        if (((uint64_t)esp_timer_get_time() - start) > DHT11_BIT_TIMEOUT_US) {
            ret    = ESP_ERR_TIMEOUT;
            reason = "总线被按住一直为低（短路 / DATA 与 GND 接反 / 传感器损坏）";
            goto done;
        }
    }
    start = (uint64_t)esp_timer_get_time();
    while (gpio_get_level(data_pin) == 1) {
        if (((uint64_t)esp_timer_get_time() - start) > DHT11_BIT_TIMEOUT_US) {
            ret    = ESP_ERR_TIMEOUT;
            reason = "应答之后的数据起始位没出现（时序被中断打断）";
            goto done;
        }
    }

    // ---- 4. 读 40 位 = 5 字节 ----
    for (int i = 0; i < 5; i++) {
        uint8_t value = 0;
        for (int bit = 0; bit < 8; bit++) {
            value = (uint8_t)((value << 1) | dht11_read_bit(data_pin));
        }
        data[i] = value;
    }

done:
    /* 单出口：保证中断一定被重新打开（否则系统会直接卡死） */
    portENABLE_INTERRUPTS();

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取失败：%s  [%s]",
                 (reason != NULL) ? reason : "未知原因", esp_err_to_name(ret));
        return ret;
    }

    /* 5 个字节全是 0 —— 校验和会"恰好"通过（0 == 0），是个经典假成功陷阱。
     * 现实中只有「线没接 / 没上拉 / 传感器没供电」才会这样。 */
    if ((data[0] | data[1] | data[2] | data[3] | data[4]) == 0) {
        ESP_LOGW(TAG, "读取失败：收到全 0（检查 DATA 上拉、供电、接线）");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t sum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (sum != data[4]) {
        ESP_LOGW(TAG, "读取失败：校验和不符（算出 0x%02X，收到 0x%02X）", sum, data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    // ---- 5. 解析 ----
    float h = (float)data[0] + (float)data[1] / 10.0f;
    float t = (float)data[2] + (float)(data[3] & 0x0F) / 10.0f;
    if ((data[3] & 0x80) != 0) {
        t = -t;                                  /* 温度符号位 */
    }

    if (t < DHT11_TEMP_SANE_MIN_C || t > DHT11_TEMP_SANE_MAX_C ||
        h < DHT11_HUMI_SANE_MIN_RH || h > DHT11_HUMI_SANE_MAX_RH) {
        ESP_LOGW(TAG, "读数超出物理合理范围，丢弃：T=%.1f°C H=%.1f%%", t, h);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *temperature = t;
    *humidity    = h;

    ESP_LOGI(TAG, "原始读数 T=%.1f°C H=%.1f%%  [字节 %02X %02X %02X %02X %02X]",
             t, h, data[0], data[1], data[2], data[3], data[4]);
    return ESP_OK;
}

// 对外入口

esp_err_t dht11_read_once(gpio_num_t data_pin, gpio_num_t power_pin,
                          float *temperature, float *humidity)
{
    if (temperature == NULL || humidity == NULL || data_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // 先清零：调用方在失败时不应该看到上次的残留值
    *temperature = 0.0f;
    *humidity    = 0.0f;

    esp_err_t err;

    // ---- 1. 上电（先把数据线放空，避免"没电时往传感器里灌电流"）----
    dht11_release_data_pin(data_pin);
    err = dht11_power_on(power_pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "传感器上电失败：%s", esp_err_to_name(err));
        dht11_safe_off(data_pin, power_pin);
        return err;
    }

    // ---- 2. 等稳定 ---- */
    vTaskDelay(pdMS_TO_TICKS(DHT11_POWER_SETTLE_MS));

    // ---- 3. 第一次读 ---- */
    err = dht11_apply_read_pin(data_pin);
    if (err == ESP_OK) {
        err = dht11_read_raw(data_pin, temperature, humidity);
    }

    // ---- 4. 失败则等 >2s 重读一次（项目书 5.4 节：断电重试一次）---- */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "第 1 次读取失败（%s），等 %d ms 后重读",
                 esp_err_to_name(err), DHT11_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(DHT11_RETRY_DELAY_MS));

        err = dht11_apply_read_pin(data_pin);
        if (err == ESP_OK) {
            err = dht11_read_raw(data_pin, temperature, humidity);
        }
    }

    // ---- 5. 无论成败，一律断电 + 释放数据线（不变量 ④）---- */
    dht11_safe_off(data_pin, power_pin);

    if (err != ESP_OK) {
        *temperature = 0.0f;
        *humidity    = 0.0f;
        ESP_LOGE(TAG, "本次读取最终失败：%s（调用方应置 sensor_ok=false，不要用上次的值）",
                 esp_err_to_name(err));
    }

    return err;
}
