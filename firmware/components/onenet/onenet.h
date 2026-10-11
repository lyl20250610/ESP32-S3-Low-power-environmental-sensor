/**
 * @file onenet.h
 * @brief OneNET Studio MQTT 上报能力（OneJSON 物模型格式）。
 *
 * ============================================================================
 * 与参考工程 AloT 的 components/MY_MQTT 相比，本组件是**减法版**
 * ============================================================================
 *
 * 删掉的（保留下来的危害大于收益）：
 *
 *   1. 重连退避定时器（5s→10s→…→120s）
 *      电池节点一次唤醒只连一次。连不上就记一笔 publish_failures 然后去睡觉 ——
 *      "下次重试"天然就是 10 分钟后的下一次唤醒。留着定时器，等于在一个
 *      本该立刻断电的窗口里持续跑射频和 TLS 重试。
 *
 *   2. 物模型下行订阅（property/set、property/query、service/+/invoke）
 *      每次订阅都是一次 TLS 往返（约 50~150ms × 3），纯增清醒时间。
 *      而设备连上后 2 秒内就去睡了，根本来不及也不打算响应下发。
 *      若将来要做"长连模式"（插电节点），另开一个组件，不要往这里塞。
 *
 *   3. 事件上报 / 服务回复 / 属性查询回复 / 属性设置回复
 *      没有下行，就没有回复；留着的代码永远不会被执行，只会增加阅读负担。
 *
 * 保留并强化的：
 *
 *   · 连接与发布的**硬超时**（ARCHITECTURE.md 不变量 ①：任何会阻塞的调用必须有上界）
 *   · QoS1 且**等到 PUBACK 才算上报成功**（决策 D4）——
 *     AloT 的 publish_checked() 只看"入队成功"，网络一断就静默丢数据。
 *   · **显式 DISCONNECT**。直接进深度睡眠而不发 DISCONNECT 的话，
 *     OneNET 会按 keepalive×1.5 继续认为这台设备在线；下一次唤醒用同一个
 *     client_id 登录时可能被拒绝或被顶下线，表现为"每轮都连不上"。
 *
 * 配置分工（ARCHITECTURE.md 第 2 节）：凭据经 `onenet_creds_t` 由调用方传入，
 * 本组件**不 include app_config.h / app_secrets.h** —— 能力层不许知道业务配置。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "onenet_token.h"

/** OneNET Studio 的 MQTT over TLS 接入点。换成旧版多协议接入或换区域时要改这里。 */
#define ONENET_MQTT_HOST                "studio-mqtts.heclouds.com"
#define ONENET_MQTT_PORT                8883

/**
 * MQTT keepalive（秒）。本组件连上后几秒内就会主动 DISCONNECT，
 * keepalive 实际上不会触发；给 60 只是为了"万一卡住了，
 * 服务端不会把我们当成幽灵在线设备太久"。
 */
#define ONENET_KEEPALIVE_SEC            60

/**
 * esp-mqtt 内部单次 TCP/TLS 建连的上限（协议/驱动级参数，归本组件）。
 * 与调用方传入的业务级 timeout_ms 是两层保护：本值 >> 业务值，
 * 保证"先由业务层放弃"，日志上看到的是我们自己的超时信息而不是驱动的。
 */
#define ONENET_NET_TIMEOUT_MS           15000

/** 发出 DISCONNECT 后等待控制权交出的小宽限，避免紧接着 destroy 把包丢掉。 */
#define ONENET_DISCONNECT_GRACE_MS      500

/**
 * token 过期时间（Unix 秒）。2524608000 ≈ 2050-01-01，等于"永久有效"。
 *
 * 为什么敢用一个远期常量：设备端没有 RTC，深睡唤醒后也无从校准时间；
 * 而每次唤醒都会**重新生成** token，所以"有效期"这个概念对本设计没有意义，
 * 给一个远期值就不必依赖时间同步。AloT 用的也是这个值。
 */
#define ONENET_TOKEN_EXPIRE_EPOCH       2524608000u

/** 设备三元组。由 main 从 app_secrets.h 取出后填进来。 */
typedef struct {
    const char *product_id;    /**< 产品 ID —— MQTT username，也是上报主题的一部分 */
    const char *device_name;   /**< 设备名  —— MQTT client_id */
    const char *device_key;    /**< 设备密钥（Base64 串），用于算 token */
} onenet_creds_t;

/**
 * @brief 连接 OneNET（阻塞，但**必有上界**）。
 *
 * 本函数负责生成 token、建 TLS、完成 MQTT CONNECT 并**等到 CONNACK 成功**，
 * 所以返回 ESP_OK 就意味着"现在可以发了"（不像 AloT 的 onenet_start 是异步的）。
 *
 * @param creds       设备三元组
 * @param timeout_ms  硬超时，必须 > 0。8883 走 TLS，握手本身要 2~4 秒。
 *
 * @return ESP_OK               已连上，可以上报
 *         ESP_ERR_TIMEOUT      超时（含 TLS 握不上、DNS 解析不出）
 *         ESP_FAIL             被服务端拒绝（CONNACK 非 0，多为 token/凭据错）
 *         ESP_ERR_INVALID_ARG  参数不合法或凭据为空
 *         ESP_ERR_NO_MEM / 其他 初始化失败
 */
esp_err_t onenet_connect(const onenet_creds_t *creds, uint32_t timeout_ms);

/** @brief 当前是否处于已连接状态。 */
bool onenet_is_connected(void);

/**
 * @brief 以 OneJSON 物模型格式上报一组属性。
 *
 * 调用方只给 `params` 的内容（例如 `"temperature":{"value":23.4},"humidity":{"value":38.0}`），
 * 报文外壳 `{"id":…,"version":"1.0","params":{…}}` 与 topic 由本组件负责。
 *
 * **QoS1 且等到 PUBACK 才返回 ESP_OK** —— 这是决策 D4 的落地：
 * 返回成功就代表平台确实收到了，可以放心把这一轮记成"上报成功"。
 *
 * @param params_json  params 对象的**内容**（不含最外层花括号）
 * @param timeout_ms   等 PUBACK 的硬超时，必须 > 0
 *
 * @return ESP_OK                平台已确认收到（PUBACK）
 *         ESP_ERR_TIMEOUT       等 PUBACK 超时（消息可能到了也可能没到）
 *         ESP_FAIL              发布入队失败，或等待期间连接断了
 *         ESP_ERR_INVALID_STATE 尚未连接
 *         ESP_ERR_INVALID_SIZE  报文超出缓冲
 */
esp_err_t onenet_publish_properties(const char *params_json, uint32_t timeout_ms);

/**
 * @brief 干净地关闭 MQTT 连接（发 DISCONNECT 再释放）。
 *
 * ⚠️ **进入深度睡眠之前必须调用**，理由见文件头注释。
 * 重复调用是安全的（内部判空直接返回）。
 */
void onenet_disconnect(void);
