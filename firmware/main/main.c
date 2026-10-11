/**
 * @file main.c
 * @brief ESP32-S3 超低功耗无线环境监测节点 —— 主程序（按 APP_DEBUG_STAGE 切换阶段）
 *
 * 当前阶段：APP_DEBUG_STAGE == 6（正式闭环：一次唤醒做完整套动作后进深睡）
 *
 * 阶段一览（app_config.h 里改一个宏就能切换）：
 *   1 = 骨架自检 + 深睡 10 秒         —— 已验证通过
 *   2 = DHT11 连续读取（每 2 秒打印） —— 已验证通过
 *   3 = 追加 SGP30 连续读数           —— 待 SGP30 到货
 *   4 = 追加 OneNET 上报（联网全链路） —— 已验证通过
 *   6 = 正式闭环（同 4 的动作 + 深睡）  —— 当前
 *
 * 为什么步骤 2~5 故意不深睡：深睡一进去串口就彻底静默，根本无法判断
 * "读数是稳定的"还是"偶尔崩一次"。调试期必须能连续观察。
 *
 * 阶段 4 与阶段 6 **共用同一套业务代码**（`node_run_cycle()`），差别只在收尾：
 * 4 是 `vTaskDelay` 后再来一轮，6 是 `esp_deep_sleep_start()` 一去不回。
 * 这不是巧合，而是刻意的 —— 步骤 6 的全部意义就是"把收尾换掉"，
 * 不是把已经验收通过的链路重写一遍。任何"两边各抄一份"的改法都会立刻
 * 让阶段 4 失去回归价值。
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
 * 板级小事：把板载 WS2812 RGB 的数据脚钉住
 *
 * 只有 15 行、只针对一个引脚，所以刻意没有单独建一个 const 组件 ——
 * 过早抽象比重复更贵。等步骤 7 出现第二个板级职责（电源门控时序）时，
 * 再把它们一起抽成 components/board。
 * =========================================================================== */

/**
 * @brief 把板载 RGB 的数据脚配成"输出 0"。
 *
 * 为什么必须管这个脚：WS2812 的 DIN 悬空时会把环境噪声当成数据，随机锁存出
 * 一个颜色 —— 三色全亮是几十 mA，足以把平均 1.65mA 的功耗预算整个打穿。
 * 它上电后从来没有人主动驱动过，"我们没碰它"不等于"它是安全的"。
 *
 * 注意：只把 DIN 拉低**不能**熄灭已经亮着的灯（WS2812 是自锁存的，只有收到
 * 24 个 0 位才会变黑）。所以正确用法是"上电第一件事就拉低"，让它永远没有
 * 机会锁存到任何东西。
 */
static void board_rgb_led_drive_low(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << APP_PIN_RGB_LED,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(APP_PIN_RGB_LED, 0));
}

/**
 * @brief 上电 / 每次唤醒都要先调它：解除上一次深睡留下的电平锁，再重新拉低。
 *
 * `gpio_deep_sleep_hold_dis()` 是"深睡期间保持电平"的总开关，
 * `gpio_hold_dis()` 才是解这一根脚的锁 —— 两个都要调。
 * 顺序不能反：解总开关 → 重新配置成输出低 → 再解这根脚的锁。
 */
static void board_rgb_led_init(void)
{
    gpio_deep_sleep_hold_dis();
    board_rgb_led_drive_low();
    gpio_hold_dis(APP_PIN_RGB_LED);
}

/* ===========================================================================
 * 阶段 4 / 6：一次完整的「采样 → 上报 → 收尾」循环
 *
 * 这一阶段把三个组件串起来跑通完整数据链路：
 *     DHT11 读数 → Wi-Fi 连上 → MQTT/TLS 连上 OneNET → 上报物模型属性 → 干净断开
 *
 * 验收标准：
 *   1. Wi-Fi 每次都能在超时内拿到 IP
 *   2. MQTT 连上 OneNET，日志出现 PUBACK 确认
 *   3. OneNET 控制台"设备属性"里能看到 temperature / humidity 跟着环境变化
 *   4. 全程没有一次"无限等待"：每个环节超时后都能自己往下走
 *   5. （仅阶段 6）睡下之后串口完全静默，再醒来时『唤醒原因 = TIMER』
 * =========================================================================== */
#if (APP_DEBUG_STAGE == 4) || (APP_DEBUG_STAGE == 6)

#include "onenet.h"
#include "wifi_sta.h"

/** 阶段 4 两轮之间的间隔（ms）。阶段 6 不用它 —— 那边的周期由深睡定时器决定。 */
#define NODE_LOOP_INTERVAL_MS   10000

/**
 * params 缓冲区。7 个非 SGP30 属性的报文实测约 210 字节，
 * 320 留足余量；超出时 onenet_publish_properties 会明确报 ESP_ERR_INVALID_SIZE。
 * 与 onenet.c 里同样的理由：声明为 static 而不是局部变量，主任务栈只有 4096 字节。
 */
#define NODE_PARAMS_MAX         320
static char s_params[NODE_PARAMS_MAX];

/**
 * 三个必须跨深度睡眠活下来的计数 —— 只能放 RTC 慢速内存。
 *
 * 普通的 .bss 变量每次唤醒都会被启动代码清零：`seq` 会永远回到 0，
 * 24 小时验收里"序号缺失"这一项统计就彻底失效了（看起来永远完美，其实什么都没测）。
 * 阶段 4 不睡，放这里也无害 —— 复位同样不丢，反而更接近真实设备的行为。
 *
 * `RTC_DATA_ATTR`（不是 `RTC_NOINIT_ATTR`）的语义恰好是我们想要的：
 * 上电复位清零，其余复位/唤醒一律保留。
 */
static RTC_DATA_ATTR uint32_t s_seq              = 0;
static RTC_DATA_ATTR uint32_t s_publish_failures = 0;
static RTC_DATA_ATTR uint32_t s_cycle            = 0;

/**
 * @brief 把唤醒原因压缩成物模型约定的 0~15 编码。
 *
 * 物模型里 wake_reason 是 int32 且限定 0~15，不能直接塞 esp_sleep_wakeup_cause_t
 * 的原始值（那些值的含义与这里的编码不是一套，以后 IDF 加新枚举就会越界）。
 * 所以显式定义映射：
 *   0 = 上电冷启动   1 = 非上电复位（烧录/看门狗/崩溃）   3 = 定时器唤醒
 *   15 = 其他（未识别）
 */
static int node_wake_reason_code(void)
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

/** 阶段 4 / 6 共用的接线与凭据提示（阶段自己的话由各自的 banner 补上）。 */
static void node_print_common_wiring(void)
{
    ESP_LOGI(TAG, "链路：DHT11(GPIO%d) → Wi-Fi → MQTT/TLS → OneNET → 上报 → 断开",
             (int)APP_PIN_DHT_DATA);
    ESP_LOGI(TAG, "接入点：%s:%d（TLS，内嵌 OneNET 自签 CA）", ONENET_MQTT_HOST, ONENET_MQTT_PORT);
    ESP_LOGI(TAG, "设备  ：产品 %s / 设备 %s", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    ESP_LOGI(TAG, "超时  ：Wi-Fi %u ms，MQTT %u ms，PUBACK %u ms（全部有上界）",
             (unsigned)APP_WIFI_TIMEOUT_MS, (unsigned)APP_MQTT_CONNECT_TIMEOUT_MS,
             (unsigned)APP_MQTT_PUBLISH_TIMEOUT_MS);

    if (secrets_look_like_placeholder(WIFI_SSID)) {
        ESP_LOGE(TAG, "⛔ WIFI_SSID 看起来还是 app_secrets.h.example 里的中文占位文本。");
        ESP_LOGE(TAG, "   请编辑 main/app_secrets.h 填入真实 2.4GHz 网络的名称和密码。");
        ESP_LOGE(TAG, "   （ESP32-S3 只支持 2.4GHz，连不上 5GHz 网络。）");
    }
}

/**
 * @brief 跑一轮完整动作：读 DHT11 → 连 Wi-Fi → 连 OneNET → 上报 → 干净断开。
 *
 * @note 故意**不含任何延时和睡眠**：收尾方式由阶段自己决定
 *       （阶段 4 用 `vTaskDelay` 再来一轮，阶段 6 直接进深睡）。
 *       本函数保证"一定能返回" —— 所有阻塞调用都有硬超时（不变量 ①）。
 */
static void node_run_cycle(void)
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
                           node_wake_reason_code(), s_publish_failures);
    if (n < 0 || n >= (int)sizeof(s_params)) {
        ESP_LOGE(TAG, "属性报文超长（需要 %d 字节，缓冲 %d）—— 本轮按上报失败处理",
                 n, (int)sizeof(s_params));
        /* 报文都没拼出来，自然谈不上上报。如实计入失败，不能悄悄跳过 ——
         * "没发出去"和"发出去没被确认"在 24 小时验收统计里的权重是一样的。 */
        s_publish_failures++;
        node_wifi_disconnect();
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
}

/* ------------------------------- 阶段 4 ------------------------------- */
#if APP_DEBUG_STAGE == 4

/**
 * 阶段 4：循环执行、不深睡。
 *
 * 它到这一步仍然有存在价值 —— 它是阶段 6 的**回归基线**：
 * 以后每次改动 node_run_cycle()，先在这里连跑十几轮看有没有退化，
 * 比直接烧阶段 6 蹲"10 分钟一轮"快一个数量级。
 */
static void stage4_loop(void)
{
    ESP_LOGI(TAG, "=========== 步骤 4/5：OneNET 联网上报测试 ===========");
    node_print_common_wiring();
    ESP_LOGI(TAG, "本阶段每 %d ms 跑一轮，**不进入深度睡眠**（方便连续观察）。",
             NODE_LOOP_INTERVAL_MS);
    ESP_LOGI(TAG, "验收：日志出现 PUBACK 确认；OneNET 控制台能看到属性变化。");
    ESP_LOGI(TAG, "=====================================================");

    while (true) {
        const int64_t t0 = esp_timer_get_time();
        node_run_cycle();
        ESP_LOGI(TAG, "本轮耗时 %" PRId64 " ms，%d ms 后开始下一轮",
                 (esp_timer_get_time() - t0) / 1000, NODE_LOOP_INTERVAL_MS);
        vTaskDelay(pdMS_TO_TICKS(NODE_LOOP_INTERVAL_MS));
    }
}

#endif /* APP_DEBUG_STAGE == 4 */

/* ------------------------------- 阶段 6 ------------------------------- */
#if APP_DEBUG_STAGE == 6

/**
 * @brief 深睡前把 RGB 数据脚锁在低电平。
 *
 * 深睡期间数字 pad 会失去驱动能力（变高阻），必须显式"锁住"电平，否则睡着的
 * 这 10 分钟里 DIN 又悬空了 —— 那正是我们要避免的情况。`gpio_hold_en()` 决定
 * 锁哪根脚，`gpio_deep_sleep_hold_en()` 才让"锁"在深睡期间生效，两个都要调。
 * 配对的解除在 board_rgb_led_init() 里。
 */
static void board_rgb_led_hold(void)
{
    board_rgb_led_drive_low();
    ESP_ERROR_CHECK(gpio_hold_en(APP_PIN_RGB_LED));
    gpio_deep_sleep_hold_en();
}

/**
 * @brief 整个固件**唯一**的出口：睡下去，不再返回。
 *
 * 为什么必须"唯一"：任何一条失败分支若忘了调它，`app_main()` 返回后
 * FreeRTOS 会走 `esp_restart()` —— 芯片重启而不是睡觉，节点变成高频重启，
 * 比不睡还费电。而这个 bug 在串口日志里看起来非常像"正常工作"：
 * 只有看电流、或看 `wake_reason` 一直是 0（上电复位）才能发现。
 *
 * 睡眠时长不是"固定睡 N 秒"，而是「目标周期 − 本轮已经用掉的时间」。
 * 固定睡 N 秒的话，真实周期会变成 N + 本轮耗时（实测 6~7 秒），
 * 600 秒的目标会变成 606~607 秒，24 小时累计漂十几分钟，
 * 项目书要求的"每 10 分钟一次上报"就不准了。
 */
static void enter_deep_sleep(void)
{
    /* esp_timer_get_time() 从本次启动的 0 开始计时，
     * 所以它此刻的值就是"被唤醒到现在"的总耗时。 */
    const int64_t awake_us = esp_timer_get_time();
    int64_t       sleep_us = (int64_t)app_sleep_seconds() * 1000000 - awake_us;

    if (sleep_us < (int64_t)APP_MIN_SLEEP_MS * 1000) {
        ESP_LOGW(TAG, "本轮已耗时 %" PRId64 " ms，达到/超过目标周期 %u 秒 —— "
                      "只睡下限 %d ms，免得变成高速空转",
                 awake_us / 1000, (unsigned)app_sleep_seconds(), APP_MIN_SLEEP_MS);
        sleep_us = (int64_t)APP_MIN_SLEEP_MS * 1000;
    }

    ESP_LOGI(TAG, "本轮唤醒→睡下共 %" PRId64 " ms；准备睡 %" PRId64 " ms（目标周期 %u 秒）",
             awake_us / 1000, sleep_us / 1000, (unsigned)app_sleep_seconds());
    ESP_LOGI(TAG, "睡眠期间串口不会有任何输出；下次醒来的『复位原因』应为 DEEPSLEEP、"
                  "『唤醒原因』应为 TIMER。");

    board_rgb_led_hold();

    /* 深睡会把没发完的日志截断（UART 有 FIFO），先把上面这几行推出去。 */
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_sleep_enable_timer_wakeup((uint64_t)sleep_us);
    esp_deep_sleep_start();

    /* 执行不到这里 —— esp_deep_sleep_start() 不返回。留着这行是为了让编译器
     * 和我们自己都不用去猜"这个函数到底有没有可能返回"。 */
    ESP_LOGE(TAG, "这行日志不应该出现：esp_deep_sleep_start() 返回了");
}

/**
 * 阶段 6：正式闭环 —— 一次唤醒做完整套动作，然后睡到下一个周期。
 *
 * 这就是全项目的最终形态：`app_main()` 不是一个常驻程序，
 * 而是"被定时器每 10 分钟执行一次的一段脚本"。
 */
static void stage6_run_once_and_sleep(void)
{
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    ESP_LOGI(TAG, "=========== 步骤 6：正式闭环（做完就睡） ===========");
    node_print_common_wiring();
    ESP_LOGI(TAG, "目标周期：%u 秒%s", (unsigned)app_sleep_seconds(),
             APP_DEBUG_SHORT_SLEEP ? "（调试期已压短，正式值见 APP_SLEEP_SECONDS）" : "");
    ESP_LOGI(TAG, "唤醒原因：%s（物模型 wake_reason 编码 %d）",
             wakeup_cause_str(cause), node_wake_reason_code());
    ESP_LOGI(TAG, "====================================================");

    node_run_cycle();

    /* 唯一的出口。node_run_cycle() 无论成功失败都会回到这里，
     * 所以"所有分支都必须汇聚"这条不变量在这里是被结构保证的，而不是靠自觉。 */
    enter_deep_sleep();
}

#endif /* APP_DEBUG_STAGE == 6 */

#endif /* 阶段 4 / 6 共用块 */

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

    /* 上电 / 每次唤醒的第一件事：把板载 RGB 的数据脚钉成低电平（理由见函数注释）。
     * 放在日志之前，是为了让"灯有没有乱亮"这个现象和我们的初始化顺序对得上。 */
    board_rgb_led_init();

    print_board_info();

#if APP_DEBUG_STAGE == 1
    stage1_sleep_demo();
#elif APP_DEBUG_STAGE == 2
    stage2_print_wiring();
    stage2_dht11_loop();
#elif APP_DEBUG_STAGE == 4
    stage4_loop();
#elif APP_DEBUG_STAGE == 6
    stage6_run_once_and_sleep();
#endif

/* 走到这里说明所选阶段没有实现。加一个编译期报错，避免"烧进去什么都不做"。
 * 阶段 3（SGP30）要等硬件到货，所以这里显式列出"已实现的阶段"——
 * 新增一个阶段就加一个分支，忘了加会直接编译失败。
 *
 * ⚠️ 阶段 6 尤其依赖这一条：万一 stage6_run_once_and_sleep() 漏了没被调用，
 * app_main() 会直接返回 → FreeRTOS 走 esp_restart() → 变成高频重启而不是深睡，
 * 而且日志上几乎看不出来。编译期挡住它，比事后拿电流表查便宜得多。 */
#if (APP_DEBUG_STAGE != 1) && (APP_DEBUG_STAGE != 2) && \
    (APP_DEBUG_STAGE != 4) && (APP_DEBUG_STAGE != 6)
#error "APP_DEBUG_STAGE 对应的阶段尚未实现（阶段 3 待 SGP30 到货）。"
#endif
}
