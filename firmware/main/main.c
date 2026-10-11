/**
 * @file main.c
 * @brief ESP32-S3 超低功耗无线环境监测节点 —— 主程序（按 APP_DEBUG_STAGE 切换阶段）
 *
 * 当前阶段：APP_DEBUG_STAGE == 2（DHT11 连续读取，不深睡）
 *
 * 阶段一览（app_config.h 里改一个宏就能切换）：
 *   1 = 骨架自检 + 深睡 10 秒         —— 已验证通过
 *   2 = DHT11 连续读取（每 2 秒打印） —— 当前
 *   3 = 追加 SGP30 连续读数
 *   4 = 追加 OneNET 上报
 *   6 = 正式闭环：一次唤醒做完整套动作后进深睡
 *
 * 为什么步骤 2~5 故意不深睡：深睡一进去串口就彻底静默，根本无法判断
 * "读数是稳定的"还是"偶尔崩一次"。调试期必须能连续观察。
 */

#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_sleep.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "dht11.h"

static const char *TAG = "env_node";

/**
 * 深睡后依然保留的计数器（存放在 RTC 内存里）。
 * 拿它来证明：RTC 内存跨深度睡眠真的活着（掉电才会清零）。
 */
RTC_DATA_ATTR static uint32_t s_boot_count = 0;

/* ===========================================================================
 * 通用工具：把枚举翻译成人话
 * =========================================================================== */

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:   return "UNKNOWN(未知)";
    case ESP_RST_POWERON:   return "POWERON(上电冷启动)";
    case ESP_RST_EXT:       return "EXT(外部复位脚)";
    case ESP_RST_SW:        return "SW(软件复位/正常烧录)";
    case ESP_RST_PANIC:     return "PANIC(程序崩溃)";
    case ESP_RST_INT_WDT:   return "INT_WDT(中断看门狗)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT(任务看门狗)";
    case ESP_RST_WDT:       return "WDT(其他看门狗)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP(深度睡眠定时唤醒)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT(欠压复位 —— 供电不足！)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "?(未识别)";
    }
}

static const char *wakeup_cause_str(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEFINED(不是深睡唤醒，是上电/复位)";
    case ESP_SLEEP_WAKEUP_TIMER:     return "TIMER(定时器到点)";
    case ESP_SLEEP_WAKEUP_GPIO:      return "GPIO";
    case ESP_SLEEP_WAKEUP_UART:      return "UART";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_EXT0:      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:      return "EXT1";
    default:                         return "?(未识别)";
    }
}

static void print_board_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t  flash_size = 0;
    esp_err_t err        = esp_flash_get_size(NULL, &flash_size);

    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "==================== 环境监测节点 ====================");
    ESP_LOGI(TAG, "固件工程 : %s  v%s", app->project_name, app->version);
    ESP_LOGI(TAG, "编译时间 : %s %s", app->date, app->time);
    ESP_LOGI(TAG, "IDF 版本 : %s", app->idf_ver);
    ESP_LOGI(TAG, "节点编号 : %s", APP_NODE_ID);

    ESP_LOGI(TAG, "芯片型号 : %s（%d 核，硅片版本 v%d.%d）",
             (chip.model == CHIP_ESP32S3) ? "ESP32-S3" : "不是 ESP32-S3！请检查 target",
             chip.cores, chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "功能位   : WiFi=%d BLE=%d 802.15.4=%d 片内Flash=%d",
             (chip.features & CHIP_FEATURE_WIFI_BGN) ? 1 : 0,
             (chip.features & CHIP_FEATURE_BLE) ? 1 : 0,
             (chip.features & CHIP_FEATURE_IEEE802154) ? 1 : 0,
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? 1 : 0);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Flash    : %" PRIu32 " MB（%" PRIu32 " 字节）—— N8R8 应为 8 MB",
                 flash_size / (1024U * 1024U), flash_size);
    } else {
        ESP_LOGW(TAG, "Flash 容量读取失败：%s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "PSRAM    : 已按设计关闭（sdkconfig 里 CONFIG_SPIRAM 未启用）");
    ESP_LOGI(TAG, "复位原因 : %s", reset_reason_str(esp_reset_reason()));
    ESP_LOGI(TAG, "唤醒原因 : %s", wakeup_cause_str(esp_sleep_get_wakeup_cause()));
    ESP_LOGI(TAG, "RTC 计数 : 第 %" PRIu32 " 次启动（深睡不清零，断电才清零）", ++s_boot_count);
    ESP_LOGI(TAG, "调试阶段 : APP_DEBUG_STAGE = %d", APP_DEBUG_STAGE);
    ESP_LOGI(TAG, "=====================================================");
}

/* ===========================================================================
 * 阶段 1：骨架自检 —— 打印硬件信息后睡 10 秒再自己醒来
 * =========================================================================== */
#if APP_DEBUG_STAGE == 1

#define STEP1_AWAKE_MS          3000
#define STEP1_SLEEP_SECONDS     10

static void stage1_sleep_demo(void)
{
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, ">> 成功：这是被【定时器】从深度睡眠里唤醒的，深睡链路已验证通过");
    } else {
        ESP_LOGI(TAG, ">> 这是冷启动（烧录/上电/复位），下面会睡下去再自己醒来");
    }

    ESP_LOGI(TAG, "保持醒着 %d ms 供你观察串口……", STEP1_AWAKE_MS);
    vTaskDelay(pdMS_TO_TICKS(STEP1_AWAKE_MS));

    ESP_LOGI(TAG, "即将进入深度睡眠 %d 秒。", STEP1_SLEEP_SECONDS);
    ESP_LOGI(TAG, "提示：睡醒后会重新打印自检信息，");
    ESP_LOGI(TAG, "      『复位原因』应显示 DEEPSLEEP、『唤醒原因』应显示 TIMER。");
    ESP_LOGI(TAG, "      正式周期 %d 秒在第 6 步接入。", APP_SLEEP_SECONDS);

    /* 给 UART 一点时间把上面的日志发完，否则最后几行会被深睡截断 */
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_sleep_enable_timer_wakeup((uint64_t)STEP1_SLEEP_SECONDS * 1000000ULL);
    esp_deep_sleep_start();
}

#endif /* APP_DEBUG_STAGE == 1 */

/* ===========================================================================
 * 阶段 2：DHT11 连续读取（对应项目书调试步骤 2）
 *
 * 验收标准：
 *   1. 连续 10 次读数全部通过 CRC 校验
 *   2. 温度在 15~35°C、湿度 20~90% 的合理区间内
 *   3. ★ 用手握住传感器，读数明显上升 —— 证明是真实测量，不是假数据
 * =========================================================================== */
#if APP_DEBUG_STAGE == 2

/** 两次读取之间的间隔（ms）。项目书 6.1 节：调试期每 2 秒打印一次。 */
#define STAGE2_INTERVAL_MS      2000

static void stage2_print_wiring(void)
{
    const gpio_num_t power_pin = app_dht_power_pin();

    ESP_LOGI(TAG, "============= 步骤 2：DHT11 连续读取测试 =============");
    if (power_pin < 0) {
        ESP_LOGI(TAG, "接线：DHT11 VCC  → 3.3V（常供）");
    } else {
        ESP_LOGI(TAG, "接线：DHT11 VCC  → GPIO%d（由固件门控供电）", (int)power_pin);
    }
    ESP_LOGI(TAG, "      DHT11 DATA → GPIO%d", (int)APP_PIN_DHT_DATA);
    ESP_LOGI(TAG, "      DHT11 GND  → GND");
    ESP_LOGI(TAG, "      3 针模块自带 4.7k~10k 上拉，不需要外接电阻");
    ESP_LOGI(TAG, "⚠️  VCC 必须接 3.3V，不要接 5V ——");
    ESP_LOGI(TAG, "    5V 会把 DATA 上拉到 5V，而 ESP32-S3 的 GPIO 不是 5V 容忍的。");
    ESP_LOGI(TAG, "本阶段每 %d ms 读一次，**不进入深度睡眠**（步骤 6 再接回深睡）。",
             STAGE2_INTERVAL_MS);
    ESP_LOGI(TAG, "验收：连续 10 次 CRC 通过；用手握住传感器，温度应明显上升。");
    ESP_LOGI(TAG, "=====================================================");
}

/**
 * @brief 接线自检：判断 DHT11 的 DATA 线上到底有没有接东西。
 *
 * 原理很简单，但能省掉大量猜测：先用**内部上拉**读一次电平，再用**内部下拉**读一次。
 *   - 上拉读 = 1，下拉读 = 1 → 外部有一个比内部下拉（约 45kΩ）更强的上拉，
 *                              也就是模块自带的 4.7k~10k 电阻 → DATA 线确实接着东西 ✅
 *   - 上拉读 = 1，下拉读 = 0 → 没有外部上拉，DATA 线基本悬空
 *                              → 大概率 DATA 没接，或接到了别的脚
 *   - 上拉读 = 0             → 被外部强行拉低 → 短路，或 DATA 接到了 GND
 *
 * 注意一个容易误判的情况：模块的上拉电阻是接到**传感器自己的 VCC** 上的。
 * 如果 VCC 那根线没接（或接到了没被驱动高的 GPIO），那么下拉读也会是 0，
 * 此时看到的"被拉低"其实是"VCC 没电"，不是短路。
 */
static void stage2_probe_data_line(gpio_num_t data_pin)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << (uint32_t)data_pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(5));
    const int with_pullup = gpio_get_level(data_pin);

    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(5));
    const int with_pulldown = gpio_get_level(data_pin);

    ESP_LOGI(TAG, "DATA 线自检：内部上拉时读到 %d，内部下拉时读到 %d", with_pullup, with_pulldown);

    if (with_pullup == 1 && with_pulldown == 1) {
        ESP_LOGI(TAG, "  ✅ 外部存在上拉电阻 —— DATA 线上确实接着东西，接线没问题");
    } else if (with_pullup == 1 && with_pulldown == 0) {
        ESP_LOGW(TAG, "  ⚠️ 没有外部上拉，DATA 线基本悬空");
        ESP_LOGW(TAG, "     检查 DATA 是否真的插在 GPIO%d 这一排，以及杜邦线是否插紧", (int)data_pin);
    } else {
        ESP_LOGW(TAG, "  ⚠️ DATA 被外部拉低");
        ESP_LOGW(TAG, "     可能是：① 传感器 VCC 没接（模块上拉失去参考电压）");
        ESP_LOGW(TAG, "             ② DATA 误接到了 GND");
        ESP_LOGW(TAG, "             ③ 杜邦线短路");
    }

    /* 自检完把线释放回"输入 + 无上拉"，保持和睡眠前一致的干净状态 */
    dht11_release_data_pin(data_pin);
}

static void stage2_dht11_loop(void)
{
    const gpio_num_t data_pin  = APP_PIN_DHT_DATA;
    const gpio_num_t power_pin = app_dht_power_pin();

    stage2_probe_data_line(data_pin);

    uint32_t total = 0;
    uint32_t ok    = 0;
    uint32_t fail  = 0;
    uint32_t range_ok_count = 0;

    while (true) {
        float t = 0.0f;
        float h = 0.0f;

        const int64_t   t0         = esp_timer_get_time();
        const esp_err_t err        = dht11_read_once(data_pin, power_pin, &t, &h);
        const int64_t   elapsed_ms = (esp_timer_get_time() - t0) / 1000;

        total++;

        if (err == ESP_OK) {
            ok++;
            const bool range_ok = (t >= APP_TEMP_MIN_C && t <= APP_TEMP_MAX_C &&
                                   h >= APP_HUMI_MIN_RH && h <= APP_HUMI_MAX_RH);
            if (range_ok) {
                range_ok_count++;
            }
            ESP_LOGI(TAG, "第 %2" PRIu32 " 次 | OK   T=%5.1f°C  H=%5.1f%% | %s | 耗时 %4" PRId64 " ms | 成功%" PRIu32 " 失败%" PRIu32,
                     total, t, h,
                     range_ok ? "范围内  " : "⚠️ 越界",
                     elapsed_ms, ok, fail);
        } else {
            fail++;
            ESP_LOGW(TAG, "第 %2" PRIu32 " 次 | FAIL %-20s | 耗时 %4" PRId64 " ms | 成功%" PRIu32 " 失败%" PRIu32,
                     total, esp_err_to_name(err), elapsed_ms, ok, fail);
        }

        /* 每 10 次打一条小结，方便一眼看出"连续 10 次"这个验收条件过没过 */
        if (total % 10 == 0) {
            ESP_LOGI(TAG, "-------------- 小结：共 %" PRIu32 " 次，成功 %" PRIu32 "，失败 %" PRIu32 "，范围合格 %" PRIu32 " --------------",
                     total, ok, fail, range_ok_count);
            fflush(stdout);
        }

        vTaskDelay(pdMS_TO_TICKS(STAGE2_INTERVAL_MS));
    }
}

#endif /* APP_DEBUG_STAGE == 2 */

/* ===========================================================================
 * 入口
 * =========================================================================== */

void app_main(void)
{
    /* gpio 驱动在每次重配引脚时都会打一条 INFO 日志。我们的读取循环每 2 秒
     * 就要重配两次引脚，不关掉的话日志会被它淹掉，真正有用的读数反而看不清。
     * 只把 gpio 这一个模块降到 WARN，WARN/ERROR 级别的信息仍然保留。 */
    esp_log_level_set("gpio", ESP_LOG_WARN);

    print_board_info();

#if APP_DEBUG_STAGE == 1
    stage1_sleep_demo();
#elif APP_DEBUG_STAGE == 2
    stage2_print_wiring();
    stage2_dht11_loop();
#endif

    /* 走到这里说明所选阶段没有实现。加一个编译期报错，避免"烧进去什么都不做"。 */
#if (APP_DEBUG_STAGE != 1) && (APP_DEBUG_STAGE != 2)
#error "APP_DEBUG_STAGE 对应的阶段尚未实现（步骤 3 起逐步补齐）。"
#endif
}
