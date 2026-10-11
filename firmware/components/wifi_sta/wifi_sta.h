/**
 * @file wifi_sta.h
 * @brief Wi-Fi STA 连接能力 —— 专为「每次唤醒连一次、报完就断」的电池节点设计。
 *
 * 与参考工程 AloT 的 components/WIFI 相比，本组件刻意**删掉**了两样东西：
 *
 *   1. 后台 10 秒周期重连定时器。
 *      电池节点醒来只连一次，连不上就记一笔失败去睡觉 ——「下次重试」天然就是
 *      10 分钟后的下一次唤醒。留着定时器只会在本该睡觉的窗口里空转射频。
 *
 *   2. xEventGroupWaitBits(..., portMAX_DELAY) 的**无限等待**。
 *      这违反 ARCHITECTURE.md 不变量 ①。只要 AP 出一次异常，
 *      节点就会被永久钉死在当前唤醒周期里，几小时内把电池耗光。
 *      本组件一律要求调用方传入有限超时。
 *
 * ⚠️ 为什么函数名不叫 wifi_sta_connect 而是 node_wifi_connect
 * ---------------------------------------------------------------------------
 * 这不是口味问题，是**链接器逼出来的**。
 *
 * 最初 API 叫 wifi_sta_connect / wifi_sta_disconnect，编译（compile）全部通过，
 * 但链接（link）阶段报：
 *
 *     libnet80211.a(ieee80211_ioctl.o): in function `wifi_sta_disconnect':
 *     multiple definition of `wifi_sta_disconnect';
 *     esp-idf/wifi_sta/libwifi_sta.a(wifi_sta.c.obj): first defined here
 *
 * ESP32 的 Wi-Fi 协议栈有一部分是**闭源预编译静态库**（libnet80211.a /
 * libcore.a / libpp.a 等），里面用了一批没有加前缀的全局符号，
 * wifi_sta_disconnect 正是其中之一。源码里完全看不出来 —— 只有在最终链接、
 * 把我们的 .o 和这些 .a 放进同一个符号表时才会撞。
 *
 * 教训：自己写的组件，公开符号一律加项目前缀。本项目约定：
 *   dht11_*       温湿度驱动
 *   node_wifi_*   Wi-Fi 连网
 *   onenet_*      OneNET 上报
 * 撞名的代价不是"编译报错"这么简单 —— 如果它不报错（比如两边都是弱符号），
 * 就会变成"我们的函数根本没被调用"这种极难查的行为异常。
 *
 * 配置分工（ARCHITECTURE.md 第 2 节「配置三处不重叠」）：
 *   协议级时序（快速重连几次）      → 本头文件的 NODE_WIFI_* 宏，一处定义
 *   业务级超时（最多等多久算失败）  → 调用方通过 timeout_ms 传入（来自 app_config.h）
 *   凭据（SSID / 密码）             → 调用方通过函数入参传入（来自 app_secrets.h）
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * 在调用方给的 timeout_ms 之内，底层最多重连几次。
 * 太小容易白跑一趟（AP 刚好在切换信道就放弃了），
 * 太大等于把超时判断交给驱动（可能一直重连到超时才算完）。
 */
#define NODE_WIFI_FAST_RETRY_MAX     3

/**
 * @brief 连接 Wi-Fi 并在拿到 IP 后返回（阻塞，但**必有上界**）。
 *
 * 成功的判据是 **拿到 IP**（IP_EVENT_STA_GOT_IP），不是"关联上了" ——
 * 只有拿到 IP 才能开始 TCP/TLS，后者才是我们真正要的。
 *
 * @param ssid        2.4GHz 网络名（ESP32-S3 不支持 5GHz）
 * @param password    密码；开放网络传 ""
 * @param timeout_ms  硬超时，必须 > 0。超时返回 ESP_ERR_TIMEOUT，**不重试、不排队**。
 *
 * @return ESP_OK          已拿到 IP，可以开始收发
 *         ESP_ERR_TIMEOUT 超时或被 AP 拒绝（调用方应放弃本周期，去睡觉）
 *         ESP_ERR_INVALID_ARG 参数不合法（含 timeout_ms == 0）
 *         其他            NVS / netif / 驱动初始化失败
 */
esp_err_t node_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * @brief 当前是否已经拿到 IP。
 * @note  可以安全地在 esp_netif_init() 之前调用（会返回 false）。
 */
bool node_wifi_is_connected(void);

/**
 * @brief 读取当前所连 AP 的信号强度。
 *
 * @param[out] rssi_dbm  信号强度（dBm，负值，越接近 0 越强）
 * @return ESP_OK           读到了
 *         ESP_ERR_INVALID_STATE 没连着
 */
esp_err_t node_wifi_get_rssi(int *rssi_dbm);

/**
 * @brief 断开并**关掉射频**。
 *
 * 为什么必须关：射频即使没连上也在持续吃几十 mA。我们的唤醒周期里
 * 「连 Wi-Fi → 上报 → 关射频 → 读 SGP30 → 睡」的顺序，正是为了把这个窗口压到最短。
 *
 * 注意本函数**不**调用 esp_wifi_deinit() —— 深度睡眠会整片断电，
 * 而调试期（不深睡）保留驱动可以省掉下一次重新初始化的一两百毫秒。
 */
void node_wifi_disconnect(void);
