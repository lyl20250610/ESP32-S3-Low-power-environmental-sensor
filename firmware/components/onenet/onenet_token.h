/**
 * @file onenet_token.h
 * @brief OneNET Studio 设备鉴权 token 生成（2018-10-31 版签名算法）。
 *
 * 为什么自己写而不用 AloT 的 components/MY_MQTT/onenet_token.c：
 *   那一份是 wolfSSL 血统的 vendored 实现，428 行里有 300 多行是自带 Base64
 *   （Base64_Encode / Base64_EncodeEsc / CEscape / DoBase64_Encode …），
 *   其中好几个函数在本工程里**永远不会被调用**。ESP-IDF 的 mbedTLS 已经提供
 *   了 base64 编解码和 HMAC，真正需要写的只有"拼字符串"这一件事，约 60 行。
 *   少 300 行死代码 = 少 300 行需要读懂和维护的东西。
 *
 * 算法（OneNET 官方文档「设备鉴权」）：
 *   1. 待签字符串 str = "{et}\n{method}\nproducts/{pid}/devices/{dev}\n2018-10-31"
 *   2. 密钥 key = Base64 解码(设备密钥)     ← 设备密钥本身就是一段 Base64 串
 *   3. sign = Base64( HMAC-{method}(key, str) )，不带换行
 *   4. token = "version=2018-10-31"
 *              "&res=products%2F{pid}%2Fdevices%2F{dev}"
 *              "&et={et}"
 *              "&method={method}"
 *              "&sign={URL编码后的 sign}"
 *
 * ⚠️ 生成出来的 token 等同于设备密码。串口日志里只许打长度或前几位，不要整串打印。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** 签名算法。OneNET 三种都支持，本项目固定用最安全的 sha256（决策见 PLAN.md 2.5）。 */
typedef enum {
    ONENET_SIG_SHA1   = 0,
    ONENET_SIG_SHA256 = 1,
} onenet_sig_t;

/**
 * token 缓冲建议大小。
 * 实测组成：固定外壳约 78 字节 + 产品 ID(10) + 设备名(6) + et(10) + method(6)
 *          + URL 编码后的 sign（SHA256 的 44 位 Base64，最坏 132 字节）≈ 250 字节。
 * 给到 320 留足余量（产品 ID / 设备名比预期长时也不会溢出）。
 */
#define ONENET_TOKEN_MAX_LEN    320

/**
 * @brief 生成设备级鉴权 token（作为 MQTT 的 password）。
 *
 * @param[out] token        输出缓冲
 * @param[in]  token_size   token 缓冲大小，建议 >= ONENET_TOKEN_MAX_LEN
 * @param[in]  method       签名算法
 * @param[in]  expire_time  过期时间（Unix 秒）。用远期时间即可，见 onenet.h 的说明。
 * @param[in]  product_id   产品 ID（MQTT username）
 * @param[in]  device_name  设备名（MQTT client_id）
 * @param[in]  device_key   设备密钥（Base64 串）
 *
 * @return ESP_OK               成功，token 以 '\0' 结尾
 *         ESP_ERR_INVALID_ARG  有空指针或空字符串（通常是凭据没填）
 *         ESP_ERR_INVALID_SIZE 待签字符串或 token 超出缓冲
 *         ESP_FAIL             设备密钥不是合法 Base64，或 HMAC 计算失败
 */
esp_err_t onenet_token_generate(char *token, size_t token_size,
                                onenet_sig_t method, uint32_t expire_time,
                                const char *product_id, const char *device_name,
                                const char *device_key);
