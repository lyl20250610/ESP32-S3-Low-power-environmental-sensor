/**
 * @file main.c
 * @brief ESP32-S3 超低功耗无线环境监测节点 —— 主程序（按 APP_DEBUG_STAGE 切换阶段）
 *
 * 当前阶段：APP_DEBUG_STAGE == 4（DHT11 + Wi-Fi + OneNET 上报，不深睡）
 *
 * 阶段一览（app_config.h 里改一个宏就能切换）：
 *   1 = 骨架自检 + 深睡 10 秒         —— 已验证通过
 *   2 = DHT11 连续读取（每 2 秒打印） —— 已验证通过
 *   3 = 追加 SGP30 连续读数           —— 待 SGP30 到货
 *   4 = 追加 OneNET 上报（联网全链路） —— 当前
 *   6 = 正式闭环：一次唤醒做完整套动作后进深睡
 *
 * 为什么步骤 2~5 故意不深睡：深睡一进去串口就彻底静默，根本无法判断
 * "读数是稳定的"还是"偶尔崩一次"。调试期必须能连续观察。
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
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

/* 凭据放在被 gitignore 的文件里（见 app_secrets.h.example 的说明）。
 * 这里做一个编译期友好提示：新克隆仓库的人如果忘了复制这个文件，
 * 报错会明确告诉他该做什么，而不是抛一句 "app_secrets.h: No such file"。 */
#if defined(__has_include)
#  if !__has_include("app_secrets.h")
#    error "缺少 main/app_secrets.h —— 请把 app_secrets.h.example 复制成 app_secrets.h 并填入真实凭据（见 README）"
#  endif
#endif
#include "app_secrets.h"

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
    ESP_LOGI(TAG, "⚠️  DATA 线上的外部上拉电阻【实测没有】，目前靠 ESP32 内部约 45kΩ 撑着");
    ESP_LOGI(TAG, "    （步骤 2 实测结论，详见 docs/HARDWARE.md §2.2；步骤 7 之前需补一只 4.7k~10k）");
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
 *                              通常就是模块自带的 4.7k~10k 电阻 → DATA 线确实接着东西 ✅
 *   - 上拉读 = 1，下拉读 = 0 → 没有外部上拉，DATA 线基本悬空
 *                              → DATA 没接、接到了别的脚，或者模块上压根没焊上拉电阻
 *   - 上拉读 = 0             → 被外部强行拉低 → 短路，或 DATA 接到了 GND
 *
 * ⚠️ 这个判据**不能**区分"没接"和"模块没有上拉电阻"——两种情况读数完全一样。
 * 2026-10-11 步骤 2 实测就是落在这个分支（上拉读 1、下拉读 0），但当时 DATA 明明
 * 接好了、读数也正常。核对 docs/HARDWARE.md §2.2 后才确认是后者：
 * 这颗模块确实没有外部上拉，靠 ESP32 内部约 45kΩ 工作。
 * 所以看到"没有外部上拉"的结论时，**先别急着怀疑接线**，用 dht11_read_once()
 * 实读一次才是决定性的证据。
 *
 * 注意另一个容易误判的情况：模块的上拉电阻是接到**传感器自己的 VCC** 上的。
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
 * 阶段 4：追加 OneNET 上报（步骤 4/5 —— 联网全链路，仍不深睡）
 *
 * 这一阶段把三个组件串起来跑通完整数据链路：
 *     DHT11 读数 → Wi-Fi 连上 → MQTT/TLS 连上 OneNET → 上报物模型属性 → 干净断开
 *
 * 验收标准：
 *   1. Wi-Fi 每次都能在超时内拿到 IP
 *   2. MQTT 连上 OneNET，日志出现 PUBACK 确认
 *   3. OneNET 控制台"设备属性"里能看到 temperature / humidity 跟着环境变化
 *   4. 全程没有一次"无限等待"：每个环节超时后都能自己往下走
 * =========================================================================== */
#if APP_DEBUG_STAGE == 4

#include "onenet.h"
#include "wifi_sta.h"

/** 两轮上报之间的间隔（ms）。调试期给足时间看日志，不打得太密。 */
#define STAGE4_INTERVAL_MS      10000

/**
 * params 缓冲区。7 个非 SGP30 属性的报文实测约 210 字节，
 * 320 留足余量；超出时 onenet_publish_properties 会明确报 ESP_ERR_INVALID_SIZE。
 * 与 onenet.c 里同样的理由：声明为 static 而不是局部变量，主任务栈只有 4096 字节。
 */
#define STAGE4_PARAMS_MAX       320
static char s_params[STAGE4_PARAMS_MAX];

/** 上报序号（seq 属性）。深睡后会由 RTC 内存接管，见阶段 6。 */
static uint32_t s_seq              = 0;
/** 累计上报失败次数（publish_failures 属性）。 */
static uint32_t s_publish_failures = 0;
/** 本地轮次计数，只用于日志排版。 */
static uint32_t s_cycle            = 0;

/**
 * @brief 把唤醒原因压缩成物模型约定的 0~15 编码。
 *
 * 物模型里 wake_reason 是 int32 且限定 0~15，不能直接塞 esp_sleep_wakeup_cause_t
 * 的原始值（那些值的含义与这里的编码不是一套，以后 IDF 加新枚举就会越界）。
 * 所以显式定义映射：
 *   0 = 上电冷启动   1 = 非上电复位（烧录/看门狗/崩溃）   3 = 定时器唤醒
 *   15 = 其他（未识别）
 */
static int stage4_wake_reason_code(void)
{
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER:
        return 3;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        return (esp_reset_reason() == ESP_RST_POWERON) ? 0 : 1;
    default:
        return 15;
    }
}

/**
 * @brief 粗判凭据是否还是模板占位文本。
 *
 * app_secrets.h.example 里的占位值是中文（"在这里填WiFi名称"），
 * 真实 SSID/密码几乎不可能是非 ASCII。所以判据取"含 >= 0x80 的字节"——
 * 不去硬编码那句中文，占位文案改了也不会漏判。
 */
static bool secrets_look_like_placeholder(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p >= 0x80) {
            return true;
        }
    }
    return false;
}

static const onenet_creds_t k_creds = {
    .product_id  = ONENET_PRODUCT_ID,
    .device_name = ONENET_DEVICE_NAME,
    .device_key  = ONENET_DEVICE_KEY,
};

static void stage4_print_wiring(void)
{
    ESP_LOGI(TAG, "=========== 步骤 4/5：OneNET 联网上报测试 ===========");
    ESP_LOGI(TAG, "链路：DHT11(GPIO%d) → Wi-Fi → MQTT/TLS → OneNET → 上报 → 断开",
             (int)APP_PIN_DHT_DATA);
    ESP_LOGI(TAG, "接入点：%s:%d（TLS，内嵌 OneNET 自签 CA）", ONENET_MQTT_HOST, ONENET_MQTT_PORT);
    ESP_LOGI(TAG, "设备  ：产品 %s / 设备 %s", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    ESP_LOGI(TAG, "超时  ：Wi-Fi %u ms，MQTT %u ms，PUBACK %u ms（全部有上界）",
             (unsigned)APP_WIFI_TIMEOUT_MS, (unsigned)APP_MQTT_CONNECT_TIMEOUT_MS,
             (unsigned)APP_MQTT_PUBLISH_TIMEOUT_MS);
    ESP_LOGI(TAG, "本阶段每 %d ms 跑一轮，**不进入深度睡眠**。", STAGE4_INTERVAL_MS);
    ESP_LOGI(TAG, "验收：日志出现 PUBACK 确认；OneNET 控制台能看到属性变化。");
    ESP_LOGI(TAG, "=====================================================");

    if (secrets_look_like_placeholder(WIFI_SSID)) {
        ESP_LOGE(TAG, "⛔ WIFI_SSID 看起来还是 app_secrets.h.example 里的中文占位文本。");
        ESP_LOGE(TAG, "   请编辑 main/app_secrets.h 填入真实 2.4GHz 网络的名称和密码。");
        ESP_LOGE(TAG, "   （ESP32-S3 只支持 2.4GHz，连不上 5GHz 网络。）");
    }
}

static void stage4_run_once(void)
{
    s_cycle++;
    ESP_LOGI(TAG, "---------- 第 %" PRIu32 " 轮开始 ----------", s_cycle);

    /* ---- 1. 读 DHT11 ---- */
    float t = 0.0f;
    float h = 0.0f;
    const int64_t   t_dht0 = esp_timer_get_time();
    const esp_err_t dht_err = dht11_read_once(APP_PIN_DHT_DATA, app_dht_power_pin(), &t, &h);
    const int64_t   dht_ms  = (esp_timer_get_time() - t_dht0) / 1000;

    const bool range_ok = (dht_err == ESP_OK) &&
                          (t >= APP_TEMP_MIN_C && t <= APP_TEMP_MAX_C) &&
                          (h >= APP_HUMI_MIN_RH && h <= APP_HUMI_MAX_RH);
    /* 读失败或读数越界时 sensor_ok = false —— 绝不用上一次的值顶替。
     * 上报的 t/h 此时是驱动清零后的 0.0，配合 sensor_ok=false 表意是明确的。 */
    const bool sensor_ok = range_ok;

    if (dht_err != ESP_OK) {
        ESP_LOGW(TAG, "DHT11 读取失败（%s，耗时 %" PRId64 " ms）",
                 esp_err_to_name(dht_err), dht_ms);
    } else if (!range_ok) {
        ESP_LOGW(TAG, "DHT11 读数越界：T=%.1f°C H=%.1f%%（合理区间 %.0f~%.0f°C / %.0f~%.0f%%）",
                 t, h, (double)APP_TEMP_MIN_C, (double)APP_TEMP_MAX_C,
                 (double)APP_HUMI_MIN_RH, (double)APP_HUMI_MAX_RH);
    } else {
        ESP_LOGI(TAG, "DHT11：T=%.1f°C  H=%.1f%%（耗时 %" PRId64 " ms）", t, h, dht_ms);
    }

    /* ---- 2. 连 Wi-Fi ---- */
    const int64_t   t_wifi0 = esp_timer_get_time();
    const esp_err_t wifi_err = node_wifi_connect(WIFI_SSID, WIFI_PASSWORD, APP_WIFI_TIMEOUT_MS);
    const int64_t   wifi_ms  = (esp_timer_get_time() - t_wifi0) / 1000;

    int rssi = 0;
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi 已连接（耗时 %" PRId64 " ms）", wifi_ms);
        if (node_wifi_get_rssi(&rssi) != ESP_OK) {
            rssi = 0;
            ESP_LOGW(TAG, "RSSI 读取失败，按 0 上报");
        } else {
            ESP_LOGI(TAG, "RSSI：%d dBm", rssi);
        }
    } else {
        ESP_LOGE(TAG, "Wi-Fi 连接失败（%s，耗时 %" PRId64 " ms）—— 本轮回报失败后直接收尾",
                 esp_err_to_name(wifi_err), wifi_ms);
    }

    /* ---- 3. 拼 OneJSON 属性 ---- */
    /* 注意 publish_failures 送的是**本轮之前**的累计值，因为"本轮是否失败"要到
     * 发布返回才知道。语义上这是对的：报文描述的是"截至发送这一刻"的状态。 */
    const int n = snprintf(s_params, sizeof(s_params),
                           "\"temperature\":{\"value\":%.1f},"
                           "\"humidity\":{\"value\":%.1f},"
                           "\"rssi\":{\"value\":%d},"
                           "\"seq\":{\"value\":%" PRIu32 "},"
                           "\"sensor_ok\":{\"value\":%s},"
                           "\"wake_reason\":{\"value\":%d},"
                           "\"publish_failures\":{\"value\":%" PRIu32 "}",
                           t, h, rssi, s_seq,
                           sensor_ok ? "true" : "false",
                           stage4_wake_reason_code(), s_publish_failures);
    if (n < 0 || n >= (int)sizeof(s_params)) {
        ESP_LOGE(TAG, "属性报文超长（需要 %d 字节，缓冲 %d）", n, (int)sizeof(s_params));
        node_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(STAGE4_INTERVAL_MS));
        return;
    }
    ESP_LOGI(TAG, "待上报：%s", s_params);

    /* ---- 4. 连 OneNET 并上报 ---- */
    bool published = false;
    if (wifi_err == ESP_OK) {
        const int64_t   t_mqtt0 = esp_timer_get_time();
        const esp_err_t mqtt_err = onenet_connect(&k_creds, APP_MQTT_CONNECT_TIMEOUT_MS);
        const int64_t   mqtt_ms  = (esp_timer_get_time() - t_mqtt0) / 1000;

        if (mqtt_err == ESP_OK) {
            ESP_LOGI(TAG, "OneNET 已连接（耗时 %" PRId64 " ms）", mqtt_ms);

            const esp_err_t pub_err =
                onenet_publish_properties(s_params, APP_MQTT_PUBLISH_TIMEOUT_MS);
            if (pub_err == ESP_OK) {
                published = true;
            } else {
                ESP_LOGE(TAG, "上报失败：%s", esp_err_to_name(pub_err));
            }
            /* 无论成功失败都要显式断开 —— 直接进深睡会让 OneNET 继续认为
             * 这台设备在线，下一轮用同一 client_id 登录可能被拒（见 onenet.h）。 */
            onenet_disconnect();
        } else {
            ESP_LOGE(TAG, "OneNET 连接失败（%s，耗时 %" PRId64 " ms）",
                     esp_err_to_name(mqtt_err), mqtt_ms);
        }
    }

    /* ---- 5. 收尾：关射频 ---- */
    node_wifi_disconnect();

    /* ---- 6. 轮次结算 ---- */
    if (published) {
        s_seq++;
        ESP_LOGI(TAG, "---------- 第 %" PRIu32 " 轮结束：上报成功（seq=%" PRIu32 "，"
                      "累计失败 %" PRIu32 "）----------",
                 s_cycle, s_seq - 1, s_publish_failures);
    } else {
        s_publish_failures++;
        ESP_LOGE(TAG, "---------- 第 %" PRIu32 " 轮结束：上报失败（累计失败 %" PRIu32 "）----------",
                 s_cycle, s_publish_failures);
    }
    fflush(stdout);

    vTaskDelay(pdMS_TO_TICKS(STAGE4_INTERVAL_MS));
}

static void stage4_loop(void)
{
    stage4_print_wiring();
    while (true) {
        stage4_run_once();
    }
}

#endif /* APP_DEBUG_STAGE == 4 */

/* ===========================================================================
 * 入口
 * =========================================================================== */

void app_main(void)
{
    /* gpio 驱动在每次重配引脚时都会打一条 INFO 日志。我们的读取循环每 2 秒
     * 就要重配两次引脚，不关掉的话日志会被它淹掉，真正有用的读数反而看不清。
     * 只把 gpio 这一个模块降到 WARN，WARN/ERROR 级别的信息仍然保留。 */
    esp_log_level_set("gpio", ESP_LOG_WARN);
    /* 同理：wifi 驱动（"mode : sta"、"sta ip"…）和 lwIP 的 DHCP 过程信息很多，
     * 而阶段 4 自己已经会把"连上了/耗时多少/失败原因"讲清楚。 */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);

    print_board_info();

#if APP_DEBUG_STAGE == 1
    stage1_sleep_demo();
#elif APP_DEBUG_STAGE == 2
    stage2_print_wiring();
    stage2_dht11_loop();
#elif APP_DEBUG_STAGE == 4
    stage4_loop();
#endif

/* 走到这里说明所选阶段没有实现。加一个编译期报错，避免"烧进去什么都不做"。
 * 阶段 3（SGP30）要等硬件到货，阶段 6（深睡闭环）排在后面 —— 都还没实现，
 * 所以这里显式列出"已实现的阶段"，新增一个阶段就加一个分支，忘了加会直接编译失败。 */
#if (APP_DEBUG_STAGE != 1) && (APP_DEBUG_STAGE != 2) && (APP_DEBUG_STAGE != 4)
#error "APP_DEBUG_STAGE 对应的阶段尚未实现（阶段 3 待 SGP30 到货，阶段 6 待步骤 6）。"
#endif
}
