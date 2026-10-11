/**
 * @file wifi_sta.c
 * @brief Wi-Fi STA 连接实现 —— 见 wifi_sta.h 说明为什么本组件是 AloT 那个的"减法版"。
 */

#include "wifi_sta.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_sta";

#define WIFI_STA_CONNECTED_BIT  BIT0    /* 已拿到 IP */
#define WIFI_STA_FAIL_BIT       BIT1    /* 快速重连耗尽，本周期放弃 */

/** netif / 事件循环 / 默认 STA 接口是否已经建好（整机只做一次） */
static bool               s_netif_ready = false;
/** esp_wifi_init 是否已经调过（整机只做一次；重复 init 会报 ESP_ERR_WIFI_INIT_STATE） */
static bool               s_wifi_inited = false;
/** 事件处理器是否已注册（重复注册会让一次断开触发多次重连） */
static bool               s_handlers_registered = false;

static EventGroupHandle_t s_events   = NULL;
static int                s_retry_num = 0;
/** 是不是"我们自己正在主动关闭射频"。
 *  esp_wifi_disconnect() 会异步抛出一条 WIFI_EVENT_STA_DISCONNECTED，
 *  如果没有这个标志，事件处理器会把它当成"意外掉线"，在关射频的当口
 *  又发起一次 esp_wifi_connect()，和紧随其后的 esp_wifi_stop() 抢时序
 *  （实测表现：每次关机都打一条"第 1/3 次快速重连"，白白多耗一次射频时间）。
 *  详见 docs/WALKTHROUGH.md §5.3 */
static volatile bool      s_shutting_down = false;

/* ===========================================================================
 * NVS —— Wi-Fi 驱动启动的硬前置
 * =========================================================================== */

/**
 * 保证 NVS 可用。Wi-Fi 驱动会把 PHY 校准数据写进 NVS，拿不到就直接启动失败。
 * 两种"NVS 用不了"的错误是可以自愈的（分区被旧版本固件塞满、或版本号不匹配），
 * 历史上标准做法就是擦掉重来；其余错误原样上报，不要瞎擦。
 */
static esp_err_t nvs_ensure_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新格式化（%s），擦除后重试", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS 擦除失败：%s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败：%s", esp_err_to_name(err));
    }
    return err;
}

/* ===========================================================================
 * 事件处理
 * =========================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* 驱动起来了，发起第一次连接 */
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)event_data;
        if (s_shutting_down) {
            /* 这是我们自己 node_wifi_disconnect() 掀起的断开，不是掉线 ——
             * 绝对不能再发起连接，否则会和 esp_wifi_stop() 抢时序。 */
            ESP_LOGD(TAG, "主动关闭引起的断开（reason=%d），按预期忽略", d ? d->reason : -1);
        } else if (s_retry_num < NODE_WIFI_FAST_RETRY_MAX) {
            s_retry_num++;
            ESP_LOGW(TAG, "AP 断开（reason=%d），第 %d/%d 次快速重连",
                     d ? d->reason : -1, s_retry_num, NODE_WIFI_FAST_RETRY_MAX);
            esp_wifi_connect();
        } else {
            /* 这里**不**启动任何后台重连定时器 —— 只告诉调用方"本周期连不上"，
             * 剩下的交给它决定（正常就是记一笔失败然后去睡觉）。 */
            ESP_LOGW(TAG, "AP 不可用（reason=%d），快速重连 %d 次仍失败，放弃本周期",
                     d ? d->reason : -1, NODE_WIFI_FAST_RETRY_MAX);
            xEventGroupSetBits(s_events, WIFI_STA_FAIL_BIT);
        }

    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "拿到 IP：" IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_events, WIFI_STA_CONNECTED_BIT);
    }
}

/** 建 netif + 默认事件循环（幂等）。顺序必须是 init → 事件循环 → 建 STA 接口 → esp_wifi_init。 */
static esp_err_t netif_and_event_loop_init(void)
{
    if (s_netif_ready) {
        return ESP_OK;
    }

    /* 这两个都是"整机一次"的初始化。重复调用会返回 ESP_ERR_INVALID_STATE，
     * 那是"别人已经做过了"，属于正常情况，不当错误处理。 */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败：%s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "默认事件循环创建失败：%s", esp_err_to_name(err));
        return err;
    }

    /* 重复调用 esp_netif_create_default_wifi_sta() 会建出第二个接口，
     * 而 wifi_is_connected 之类的按名字查找只会命中第一个 —— 所以先查再建。 */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGE(TAG, "创建默认 STA 网络接口失败");
            return ESP_FAIL;
        }
    }

    s_netif_ready = true;
    return ESP_OK;
}

static esp_err_t handlers_register_once(void)
{
    if (s_handlers_registered) {
        return ESP_OK;
    }
    esp_err_t err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 WIFI_EVENT 处理器失败：%s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 IP_EVENT 处理器失败：%s", esp_err_to_name(err));
        return err;
    }
    s_handlers_registered = true;
    return ESP_OK;
}

/* ===========================================================================
 * 公共 API
 * =========================================================================== */

bool node_wifi_is_connected(void)
{
    /* 注意：本函数可能在 esp_netif_init() 之前被调用（用于快速路径判断），
     * esp_netif_get_handle_from_ifkey 在未初始化时安全地返回 NULL。 */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return false;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
        return false;
    }
    return esp_netif_is_netif_up(netif);
}

esp_err_t node_wifi_get_rssi(int *rssi_dbm)
{
    if (rssi_dbm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!node_wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_ap_record_t ap = {0};
    const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) {
        return err;
    }
    *rssi_dbm = ap.rssi;
    return ESP_OK;
}

esp_err_t node_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 不变量 ①：这里必须挡住"无限等待"，否则整机可能被钉死一个周期。 */
    if (timeout_ms == 0) {
        ESP_LOGE(TAG, "timeout_ms 不能为 0（本工程禁止无限等待）");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            ESP_LOGE(TAG, "创建事件组失败");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 快速路径：调试期（不深睡）反复调用时，已经连着就直接复用，
     * 免得白白断一次再连一次。 */
    if (s_wifi_inited && node_wifi_is_connected() && s_retry_num == 0) {
        ESP_LOGI(TAG, "已经连着，复用当前连接");
        return ESP_OK;
    }

    /* 清掉上一次的残留位，否则"上一轮的 FAIL"会让这一轮立刻误判失败 */
    xEventGroupClearBits(s_events, WIFI_STA_CONNECTED_BIT | WIFI_STA_FAIL_BIT);
    s_retry_num = 0;
    /* 本轮是"要连接"，撤销上一轮关机时立起的标志（必须在 esp_wifi_start() 之前，
     * 否则 STA_START/DISCONNECTED 事件会被误判成"正在关机"而不重连） */
    s_shutting_down = false;

    esp_err_t err = nvs_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    err = netif_and_event_loop_init();
    if (err != ESP_OK) {
        return err;
    }
    err = handlers_register_once();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_wifi_inited) {
        const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init 失败：%s", esp_err_to_name(err));
            return err;
        }
        s_wifi_inited = true;
    }

    /* ssid / password 在 wifi_config_t 里是定长数组，运行期变量必须 strncpy，
     * 且要手工补 '\0' —— strncpy 在源串长度 >= 目标大小时不会补结束符。 */
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    /* threshold.authmode 是"可接受的最低加密等级"。这里取 WIFI_AUTH_OPEN（=0，不设限），
     * 让驱动自己按 AP 通告的能力协商 —— WPA / WPA2 / WPA2-WPA3 混合 / WPA3 全都能连。
     * AloT 那边写死成 WPA2_PSK，遇到只开 WPA(混合) 或 WPA3-only 的路由器会表现成
     * "密码明明是对的却连不上"，排查起来非常费劲。本项目要的是"接上就行"，
     * 安全性由 Wi-Fi 本身提供的加密保证，不需要在这里做筛选。 */
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 STA 模式失败：%s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 Wi-Fi 配置失败：%s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "连接 \"%s\" ……（硬超时 %u ms，最多重连 %d 次）",
             ssid, (unsigned)timeout_ms, NODE_WIFI_FAST_RETRY_MAX);

    const int64_t t_start_us = esp_timer_get_time();

    err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_STATE) {
        /* 已经启动过（调试期重复调用会走到这里）。此时不会再收到
         * WIFI_EVENT_STA_START，必须自己发起连接，否则会干等到超时。 */
        ESP_LOGI(TAG, "射频已处于启动状态，直接发起连接");
        esp_wifi_connect();
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败：%s", esp_err_to_name(err));
        return err;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_events,
        WIFI_STA_CONNECTED_BIT | WIFI_STA_FAIL_BIT,
        pdFALSE,   /* 不清零：后续步骤可能还要靠它判断状态 */
        pdFALSE,   /* 任一位满足即返回 */
        pdMS_TO_TICKS(timeout_ms));

    const int64_t elapsed_ms = (esp_timer_get_time() - t_start_us) / 1000;

    if (bits & WIFI_STA_CONNECTED_BIT) {
        ESP_LOGI(TAG, "连接成功，耗时 %" PRId64 " ms", elapsed_ms);
        return ESP_OK;
    }

    if (bits & WIFI_STA_FAIL_BIT) {
        ESP_LOGE(TAG, "连接失败：快速重连已耗尽（累计 %" PRId64 " ms）—— "
                      "常见原因：密码错（reason=15/205）、5GHz 网络（ESP32-S3 只支持 2.4GHz）、"
                      "AP 开了 MAC 白名单", elapsed_ms);
    } else {
        ESP_LOGE(TAG, "连接超时：等了 %" PRId64 " ms，上限 %u ms",
                 elapsed_ms, (unsigned)timeout_ms);
    }
    return ESP_ERR_TIMEOUT;
}

void node_wifi_disconnect(void)
{
    if (!s_wifi_inited) {
        return;
    }

    /* 先立标志：esp_wifi_disconnect() 会异步抛 DISCONNECTED 事件，
     * 标志必须先于它置位，事件处理器才不会把它当成掉线去重连。 */
    s_shutting_down = true;

    /* 先 disconnect（礼貌地通知 AP），再 stop（真正关射频）。 */
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_disconnect 返回：%s", esp_err_to_name(err));
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop 返回：%s", esp_err_to_name(err));
    }

    xEventGroupClearBits(s_events, WIFI_STA_CONNECTED_BIT | WIFI_STA_FAIL_BIT);
    ESP_LOGI(TAG, "射频已关闭（本轮 Wi-Fi 结束）");
}
