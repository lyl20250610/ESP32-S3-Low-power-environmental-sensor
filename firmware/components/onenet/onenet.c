/**
 * @file onenet.c
 * @brief OneNET Studio MQTT 上报实现 —— 设计取舍见 onenet.h。
 */

#include "onenet.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

static const char *TAG = "onenet";

/* ===========================================================================
 * OneNET 服务端 CA 证书
 * ===========================================================================
 *
 * 为什么内嵌 PEM，而不用 ESP-IDF 自带的 Mozilla CA bundle（本工程
 * CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y，本来是可以用的）：
 *
 *   这两张是 OneNET **自签**的根证书 —— 主体 CN 分别是 "OneNET MQ" 和
 *   "OneNET MQTTS"，签发者是 CMIOT，有效期 2019→2049。它们**不在** Mozilla
 *   的公共信任列表里，所以 esp_crt_bundle_attach 验证必然失败。
 *   与 AloT 参考工程保持一致的取值。
 *
 * ---------------------------------------------------------------------------
 * 为什么必须跳过 CN 校验（skip_cert_common_name_check = true）
 * ---------------------------------------------------------------------------
 *
 * 2026-10-11 在 PC 上实测（见 tools/onenet_token_check.py）：
 *
 *   服务器实际出示的证书 = **自签名**，subject = issuer = CN "OneNET MQTTS"、
 *   O "CMIOT"，**完全没有 subjectAltName 扩展**，有效期 2019-05-29 → 2049-05-21。
 *   无论 SNI 传什么、无论 TLS 1.2 还是 1.3，服务器都给同一张。
 *
 * 而 mbedTLS 的判定（mbedtls/library/x509_crt.c:2991-3005）是：
 *   证书有 SAN → 比 SAN；**没有 SAN → 回退比 CN**；两者都不符就置
 *   MBEDTLS_X509_BADCERT_CN_MISMATCH。
 * esp-tls 只要 mbedtls_ssl_get_verify_result() != 0 就判握手失败
 * （esp-tls/esp_tls_mbedtls.c:351）。
 *
 * 于是：CN 是 "OneNET MQTTS" ≠ "studio-mqtts.heclouds.com" → **握手永远失败，
 * 根本走不到 MQTT**。AloT 参考工程里写的 false，在这条链路上是走不通的。
 *
 * 关掉它损失什么？什么都不损失。信任本来是由**上面这张钉死的 CA** 建立的
 * —— 证书链仍然被严格校验，自签证书必须与内嵌 PEM 完全一致才放行。
 * 对一张自签、且 CN 根本不是主机名的证书来说，CN 校验提供不了任何额外的身份
 * 保证，只会挡路。这和"关掉证书校验"是两件完全不同的事，后者才是危险的。
 *
 * 维护提示：OneNET 若更换服务端证书，这里的内嵌 PEM 要跟着换。**不要**为了
 * 图省事改走明文 1883 —— 那等于把设备密钥和 token 明文发到公网上
 * （决策 D4 选 8883 TLS 就是为了避免这个）。
 */
static const char ONENET_CA_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDNTCCAh2gAwIBAgIJAI0j4gYGUcD2MA0GCSqGSIb3DQEBCwUAMDExCzAJBgNV\n"
    "BAYTAkNOMQ4wDAYDVQQKDAVDTUlPVDESMBAGA1UEAwwJT25lTkVUIE1RMB4XDTE5\n"
    "MDYxMzAyMTgzM1oXDTQ5MDYwNTAyMTgzM1owMTELMAkGA1UEBhMCQ04xDjAMBgNV\n"
    "BAoMBUNNSU9UMRIwEAYDVQQDDAlPbmVORVQgTVEwggEiMA0GCSqGSIb3DQEBAQUA\n"
    "A4IBDwAwggEKAoIBAQDrSdCdWHPwgB1KFo6Gb3nbjVYMbx+3hQ1Qdf+M4+i8MgYz\n"
    "KSdWTvBf5+eSDEPhAMf7CDbfLtqpEKJPQ8MqcqwRFn5Ahux+bfgzV8QHwsbxqkPz\n"
    "Z8Ga/2PkL3fJM1Cunq1o4WNRNwgWI0JQbqwEofhuUasJuq63jE7bdzIE60eEjsFw\n"
    "Tc6ZiRZf9DzQDfI3v0wOKTTfyUlJZgBpQouJBFio9cMW15DvqcT7VmDHd9+gppBa\n"
    "Wq6Nak+EverpyWW8TaqnwIQRdRwxXu6f8iNQb5UlgW2cUP7pZu0xIY+sE4E94mpV\n"
    "hZHEmbO3Y8x9Q1r5YwG0oTz+UfCI+toh9OIV2msjAgMBAAGjUDBOMB0GA1UdDgQW\n"
    "BBSBOGqKT3bVuY6bYlaCfEwryMarIzAfBgNVHSMEGDAWgBSBOGqKT3bVuY6bYlaC\n"
    "fEwryMarIzAMBgNVHRMEBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQBwSZz/bbpF\n"
    "yNudik/ZkVIiGkTDC0iPQruZxEghX/UMVSJEpOMOOTz9ws5WUb7B3XWRk85L0YRC\n"
    "Vh9Axcs7zAiPsVVvPmYO0GkWYzP6MedLyXHMLuoqHSRC6NlgyITXGe7vrClF7YSG\n"
    "C0nStWF1kQ48RX17Ty5gCsIO/21PIVeXxDXWNhMGs22cLGUR2udGwJQdG5q0ZJwY\n"
    "hsVDcAQCOP2nuGivh6cP+2acXQH+6C1KMWV4vIk63cC4G1VgXV4ai+Glf6riA8jO\n"
    "txrtp4v0uwUerArO+wtX/2YXaQWbhYs5+K0dOIi8TltVWiXxyXKqYSsjFbSq0h7F\n"
    "KZ2I6wtffJnH\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDOzCCAiOgAwIBAgIJAPCCNfxANtVEMA0GCSqGSIb3DQEBCwUAMDQxCzAJBgNV\n"
    "BAYTAkNOMQ4wDAYDVQQKDAVDTUlPVDEVMBMGA1UEAwwMT25lTkVUIE1RVFRTMB4X\n"
    "DTE5MDUyOTAxMDkyOFoXDTQ5MDUyMTAxMDkyOFowNDELMAkGA1UEBhMCQ04xDjAM\n"
    "BgNVBAoMBUNNSU9UMRUwEwYDVQQDDAxPbmVORVQgTVFUVFMwggEiMA0GCSqGSIb3\n"
    "DQEBAQUAA4IBDwAwggEKAoIBAQC/VvJ6lGWfy9PKdXKBdzY83OERB35AJhu+9jkx\n"
    "5d4SOtZScTe93Xw9TSVRKrFwu5muGgPusyAlbQnFlZoTJBZY/745MG6aeli6plpR\n"
    "r93G6qVN5VLoXAkvqKslLZlj6wXy70/e0GC0oMFzqSP0AY74icANk8dUFB2Q8usS\n"
    "UseRafNBcYfqACzF/Wa+Fu/upBGwtl7wDLYZdCm3KNjZZZstvVB5DWGnqNX9HkTl\n"
    "U9NBMS/7yph3XYU3mJqUZxryb8pHLVHazarNRppx1aoNroi+5/t3Fx/gEa6a5PoP\n"
    "ouH35DbykmzvVE67GUGpAfZZtEFE1e0E/6IB84PE00llvy3pAgMBAAGjUDBOMB0G\n"
    "A1UdDgQWBBTTi/q1F2iabqlS7yEoX1rbOsz5GDAfBgNVHSMEGDAWgBTTi/q1F2ia\n"
    "bqlS7yEoX1rbOsz5GDAMBgNVHRMEBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQAL\n"
    "aqJ2FgcKLBBHJ8VeNSuGV2cxVYH1JIaHnzL6SlE5q7MYVg+Ofbs2PRlTiWGMazC7\n"
    "q5RKVj9zj0z/8i3ScWrWXFmyp85ZHfuo/DeK6HcbEXJEOfPDvyMPuhVBTzuBIRJb\n"
    "41M27NdIVCdxP6562n6Vp0gbE8kN10q+ksw8YBoLFP0D1da7D5WnSV+nwEIP+F4a\n"
    "3ZX80bNt6tRj9XY0gM68mI60WXrF/qYL+NUz+D3Lw9bgDSXxpSN8JGYBR85BxBvR\n"
    "NNAhsJJ3yoAvbPUQ4m8J/CoVKKgcWymS1pvEHmF47pgzbbjm5bdthlIx+swdiGFa\n"
    "WzdhzTYwVkxBaU+xf/2w\n"
    "-----END CERTIFICATE-----\n";

/* ===========================================================================
 * 事件位与静态状态
 * =========================================================================== */

#define BIT_CONNECTED     BIT0   /* 收到 CONNACK 且 connack 返回码为 0 */
#define BIT_DISCONNECTED  BIT1   /* 连接断开（含连上之前就被拒） */
#define BIT_PUBACKED      BIT2   /* 上一次发布的报文拿到了 PUBACK */

/** 事件组指针在外部尚未做任何网络初始化时是 NULL，事件回调里必须先判空。 */
static EventGroupHandle_t      s_events    = NULL;
static esp_mqtt_client_handle_t s_client   = NULL;
static bool                    s_connected = false;

/** 已收到 PUBACK 的报文 id。用"记下最近一次"的方式回避与发布函数之间的竞态。 */
static int  s_pubacked_msg_id = -1;
/** 递增序号，保证同一毫秒内连发多次也不会撞 id。 */
static uint32_t s_msg_seq = 0;

/**
 * 产品 ID 与设备名在 connect 时存下来，供 onenet_publish_properties() 拼主题用。
 * 只存指针不拷贝：调用方传进来的是 app_secrets.h 里的字符串字面量，生命周期是全程。
 */
static const char *s_product_id  = NULL;
static const char *s_device_name = NULL;

/**
 * ⚠️ 这两个缓冲刻意声明为 static 而不是局部变量：
 *    app_main 所在任务的栈只有 CONFIG_ESP_MAIN_TASK_STACK_SIZE = 4096 字节，
 *    而报文最长可能到 400 字节以上，再加 mqtt 内部调用的栈开销，
 *    放栈上有溢出风险。它们只在主任务里被顺序使用，不存在重入问题。
 */
static char s_token[ONENET_TOKEN_MAX_LEN];
static char s_topic[128];
static char s_payload[512];

static void onenet_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    (void)arg;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT 已连接（session_present=%d）", event->session_present);
        s_connected = true;
        /* 一起置位 DISCONNECTED 的对位清零，免得上一轮的断开状态干扰本轮判断 */
        xEventGroupClearBits(s_events, BIT_DISCONNECTED);
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT 连接断开");
        s_connected = false;
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
        break;

    case MQTT_EVENT_PUBLISHED:
        /* QoS1 的 PUBACK 回来了 —— 到这一步平台才算真正确认收到。 */
        s_pubacked_msg_id = event->msg_id;
        xEventGroupSetBits(s_events, BIT_PUBACKED);
        break;

    case MQTT_EVENT_ERROR: {
        /* AloT 在这里只打了一句 "MQTT error occurred"。
         * 连不上是这一步最常见的故障，把结构化字段全打出来能省掉一整轮排查。 */
        const esp_mqtt_error_codes_t *e = event->error_handle;
        if (e == NULL) {
            ESP_LOGE(TAG, "MQTT 错误（无详情）");
            break;
        }
        ESP_LOGE(TAG, "MQTT 错误：type=%d tls_last_err=0x%x tls_stack_err=%d "
                      "sock_errno=%d connect_return_code=%d",
                 (int)e->error_type, (unsigned)e->esp_tls_last_esp_err,
                 (int)e->esp_tls_stack_err, (int)e->esp_transport_sock_errno,
                 (int)e->connect_return_code);
        /* 补一句人话，把最常见的两种成因点出来 */
        if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "  → 服务端拒绝连接：return_code=1 版本不支持 / 4 用户名密码错 / 5 未授权。"
                          "本项目几乎总是 token 或产品 ID、设备名、设备密钥不匹配");
        } else if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  → TCP/TLS 层失败：先确认 Wi-Fi 已拿到 IP 且能解析 "
                          "studio-mqtts.heclouds.com（DNS 不通也会走到这里）");
        }
        break;
    }

    default:
        break;
    }
}

/* ===========================================================================
 * 公共 API
 * =========================================================================== */

bool onenet_is_connected(void)
{
    return s_connected;
}

esp_err_t onenet_connect(const onenet_creds_t *creds, uint32_t timeout_ms)
{
    if (creds == NULL || creds->product_id == NULL ||
        creds->device_name == NULL || creds->device_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 不变量 ①：任何会阻塞的调用必须有上界，禁止"等一辈子" */
    if (timeout_ms == 0) {
        ESP_LOGE(TAG, "timeout_ms 不能为 0（本工程禁止无限等待）");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_connected && s_client != NULL) {
        ESP_LOGI(TAG, "已经连着，复用当前连接");
        return ESP_OK;
    }

    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            ESP_LOGE(TAG, "创建事件组失败");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 每次唤醒都用新 token。设备端没有 RTC，token 的"有效期"对本设计没有意义，
     * 给一个远期时间（2050）就永远不用考虑续期。 */
    esp_err_t err = onenet_token_generate(s_token, sizeof(s_token), ONENET_SIG_SHA256,
                                          ONENET_TOKEN_EXPIRE_EPOCH,
                                          creds->product_id, creds->device_name,
                                          creds->device_key);
    if (err != ESP_OK) {
        return err;
    }
    /* 只打长度，不打内容 —— 这串东西等同于设备密码。 */
    ESP_LOGI(TAG, "token 已生成（%u 字符）", (unsigned)strlen(s_token));

    s_product_id  = creds->product_id;
    s_device_name = creds->device_name;

    /* 上一轮如果没走 disconnect（比如上次是复位死的），把残留句柄清掉，
     * 否则 esp_mqtt_client_init 会再建一套任务和 socket。 */
    if (s_client != NULL) {
        ESP_LOGW(TAG, "发现残留的 MQTT 客户端句柄，先销毁");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .hostname  = ONENET_MQTT_HOST,
                .port      = ONENET_MQTT_PORT,
                .transport = MQTT_TRANSPORT_OVER_SSL,
            },
            .verification = {
                .certificate = ONENET_CA_CERT,
                /* ⚠️ **必须**为 true —— 理由见文件头「为什么必须跳过 CN 校验」。
                 * 一句话：OneNET 的证书自签、无 SAN、CN 写的是 "OneNET MQTTS"
                 * 而非主机名；mbedTLS 无 SAN 时回退比 CN，不符即置
                 * BADCERT_CN_MISMATCH，esp-tls 见到 verify_result != 0 就判握手
                 * 失败，于是永远走不到 MQTT。信任由上面那张钉死的 CA 提供。 */
                .skip_cert_common_name_check = true,
            },
        },
        .credentials = {
            .username = creds->product_id,   /* OneNET：username = 产品 ID */
            .client_id = creds->device_name, /* OneNET：client_id = 设备名 */
            .authentication = {
                .password = s_token,         /* OneNET：password = 鉴权 token */
            },
        },
        .session = {
            .keepalive             = ONENET_KEEPALIVE_SEC,
            .disable_clean_session = false,
            .protocol_ver          = MQTT_PROTOCOL_V_3_1_1,
        },
        .network = {
            .timeout_ms            = ONENET_NET_TIMEOUT_MS,
            /* 关键：关掉 esp-mqtt 自带的重连。断线后由本组件把失败上报给调用方，
             * 调用方记一笔然后去睡觉 —— "下次重试"就是 10 分钟后的下一次唤醒。 */
            .disable_auto_reconnect = true,
        },
        .task = {
            .stack_size = 4096,
            .priority   = 5,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init 失败");
        return ESP_FAIL;
    }

    err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, onenet_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 MQTT 事件处理器失败：%s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_DISCONNECTED | BIT_PUBACKED);

    const int64_t t_start_us = esp_timer_get_time();

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start 失败：%s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    /* 同步等到 CONNACK —— 与 AloT 的 onenet_start()（异步、返回后还得自己轮询）
     * 不同：这里返回 ESP_OK 就意味着"现在可以发了"。 */
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_CONNECTED | BIT_DISCONNECTED,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    const int64_t elapsed_ms = (esp_timer_get_time() - t_start_us) / 1000;

    if (bits & BIT_CONNECTED) {
        ESP_LOGI(TAG, "连接成功，耗时 %" PRId64 " ms", elapsed_ms);
        return ESP_OK;
    }

    if (bits & BIT_DISCONNECTED) {
        ESP_LOGE(TAG, "连接被拒或中途断开（%s）", "详见上面的事件错误详情");
    } else {
        ESP_LOGE(TAG, "连接超时：等了 %" PRId64 " ms（上限 %u ms），"
                      "TLS 握手通常需要 2~4 秒",
                 elapsed_ms, (unsigned)timeout_ms);
    }

    /* 失败也要把客户端收干净，否则下一次唤醒会多出一套任务和 socket。 */
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    return (bits & BIT_DISCONNECTED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

esp_err_t onenet_publish_properties(const char *params_json, uint32_t timeout_ms)
{
    if (params_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        ESP_LOGE(TAG, "timeout_ms 不能为 0（本工程禁止无限等待）");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client == NULL || !s_connected) {
        ESP_LOGE(TAG, "尚未连接 OneNET，无法上报");
        return ESP_ERR_INVALID_STATE;
    }

    /* msg_id 里带毫秒时间戳 + 自增序号：既能在日志里看出"这是哪一轮"，
     * 又能保证同一毫秒内连发多条也不会撞 id（撞 id 会让 PUBACK 对不上号）。 */
    char msg_id[32];
    snprintf(msg_id, sizeof(msg_id), "%" PRIu64 "%04x",
             (uint64_t)(esp_timer_get_time() / 1000), (unsigned)(s_msg_seq++ & 0xFFFF));

    const int topic_len = snprintf(s_topic, sizeof(s_topic),
                                   "$sys/%s/%s/thing/property/post",
                                   s_product_id, s_device_name);
    if (topic_len < 0 || topic_len >= (int)sizeof(s_topic)) {
        ESP_LOGE(TAG, "上报主题超长（需要 %d 字节）", topic_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* OneJSON 物模型报文的固定外壳：id + version + params。 */
    const int payload_len = snprintf(s_payload, sizeof(s_payload),
                                     "{\"id\":\"%s\",\"version\":\"1.0\",\"params\":{%s}}",
                                     msg_id, params_json);
    if (payload_len < 0 || payload_len >= (int)sizeof(s_payload)) {
        ESP_LOGE(TAG, "上报报文超长（需要 %d 字节，缓冲 %d）—— "
                      "本轮属性太多了，考虑拆成多次发布", payload_len, (int)sizeof(s_payload));
        return ESP_ERR_INVALID_SIZE;
    }

    /* 清 PUBACK 位要在发布之前 —— 否则上一轮遗留的置位会让本轮误判"已确认"。 */
    xEventGroupClearBits(s_events, BIT_PUBACKED);
    s_pubacked_msg_id = -1;

    /* QoS1（第 5 个参数 = 1）、retain=0。AloT 用的也是这两个值。 */
    const int pub_msg_id = esp_mqtt_client_publish(s_client, s_topic, s_payload,
                                                   payload_len, 1, 0);
    if (pub_msg_id < 0) {
        ESP_LOGE(TAG, "发布入队失败（msg_id=%d）：发送队列满或客户端正在关闭", pub_msg_id);
        return ESP_FAIL;
    }

    const int64_t t_start_us = esp_timer_get_time();

    /* 等到 PUBACK 才算成功（决策 D4）。AloT 的 publish_checked() 只看"入队成功"
     * 就返回 OK —— 网络一断数据就静默丢了，而调用方还以为上报成功了。 */
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_PUBACKED | BIT_DISCONNECTED,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    const int64_t elapsed_ms = (esp_timer_get_time() - t_start_us) / 1000;

    if (bits & BIT_PUBACKED) {
        /* 竞态说明：PUBACK 理论上可能在 esp_mqtt_client_publish() 返回之前就到了
         * （msg_id 在入队时分配，网络任务可以并行把包发出去）。
         * 那种情况下事件里已经记下了 id，这里比对 id 即可正确处理。 */
        if (s_pubacked_msg_id != pub_msg_id) {
            ESP_LOGW(TAG, "收到的是另一条报文的 PUBACK（期望 %d，实际 %d），"
                          "本轮按未确认处理", pub_msg_id, s_pubacked_msg_id);
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGI(TAG, "上报已确认（PUBACK msg_id=%d，耗时 %" PRId64 " ms）",
                 pub_msg_id, elapsed_ms);
        return ESP_OK;
    }

    if (bits & BIT_DISCONNECTED) {
        ESP_LOGE(TAG, "等待 PUBACK 期间连接断开（%d 字节的报文可能已发出也可能没有）",
                 payload_len);
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "等 PUBACK 超时：等了 %" PRId64 " ms（上限 %u ms）",
             elapsed_ms, (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

void onenet_disconnect(void)
{
    if (s_client == NULL) {
        s_connected = false;
        return;
    }

    /* 显式 DISCONNECT 不是可选项。直接进深度睡眠的话，OneNET 会按
     * keepalive×1.5 继续认为这台设备在线；下一次唤醒用同一个 client_id
     * 登录时可能被拒绝，表现为"每轮都连不上"，而且日志上看不出原因。 */
    if (s_connected) {
        const esp_err_t err = esp_mqtt_client_disconnect(s_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_mqtt_client_disconnect 返回：%s", esp_err_to_name(err));
        }
        /* 给协议栈一点时间把这个包真正写进 socket 再拆连接 */
        vTaskDelay(pdMS_TO_TICKS(ONENET_DISCONNECT_GRACE_MS));
    }

    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client    = NULL;
    s_connected = false;
    ESP_LOGI(TAG, "MQTT 会话已关闭");
}
