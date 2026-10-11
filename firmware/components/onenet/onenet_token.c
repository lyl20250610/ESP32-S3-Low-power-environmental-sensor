/**
 * @file onenet_token.c
 * @brief OneNET 设备鉴权 token 生成实现 —— 算法说明见 onenet_token.h。
 *
 * 全部工作交给 mbedTLS：base64 解码/编码 + HMAC。本文件只负责拼字符串。
 */

#include "onenet_token.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static const char *TAG = "onenet_token";

#define ONENET_TOKEN_VERSION    "2018-10-31"

/** 待签字符串上限。实测约 65 字节（10+1+6+1+8+10+9+7+1+10），160 很宽裕。 */
#define SIG_TEXT_MAX            160
/** Base64 解码后的设备密钥上限。OneNET 设备密钥一般 44 位 Base64 → 32 字节。 */
#define DEV_KEY_MAX             96

static const char *sig_method_name(onenet_sig_t method)
{
    return (method == ONENET_SIG_SHA256) ? "sha256" : "sha1";
}

static mbedtls_md_type_t sig_md_type(onenet_sig_t method)
{
    return (method == ONENET_SIG_SHA256) ? MBEDTLS_MD_SHA256 : MBEDTLS_MD_SHA1;
}

/**
 * @brief 把一个字节按 OneNET 要求做 URL 编码写进 out。
 *
 * 只编码会破坏 query string 结构的字符 —— 全编码虽然也"能用"，
 * 但会让 token 变长、日志更难读，而且 OneNET 服务端并不要求全编码。
 *
 * @return 写入了几个字节（1 或 3）。out 至少要有 3 字节空间。
 */
static size_t url_encode_byte(char *out, unsigned char c)
{
    const char *esc = NULL;
    switch (c) {
    case '+': esc = "%2B"; break;
    case ' ': esc = "%20"; break;
    case '/': esc = "%2F"; break;
    case '?': esc = "%3F"; break;
    case '%': esc = "%25"; break;
    case '#': esc = "%23"; break;
    case '&': esc = "%26"; break;
    case '=': esc = "%3D"; break;
    default: break;
    }
    if (esc != NULL) {
        memcpy(out, esc, 3);
        return 3;
    }
    out[0] = (char)c;
    return 1;
}

esp_err_t onenet_token_generate(char *token, size_t token_size,
                                onenet_sig_t method, uint32_t expire_time,
                                const char *product_id, const char *device_name,
                                const char *device_key)
{
    if (token == NULL || token_size == 0 ||
        product_id == NULL || device_name == NULL || device_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 凭据没填时给出指名道姓的提示，而不是生成一个连不上的 token 让人去猜。
     * （第一次 clone 后最常见的状态就是 app_secrets.h 还是模板占位文本。） */
    if (product_id[0] == '\0' || device_name[0] == '\0' || device_key[0] == '\0') {
        ESP_LOGE(TAG, "凭据为空 —— 请检查 app_secrets.h 里的产品 ID / 设备名 / 设备密钥");
        return ESP_ERR_INVALID_ARG;
    }
    token[0] = '\0';

    /* ---- 第 1 步：待签字符串 ---- */
    char sig_text[SIG_TEXT_MAX];
    const int sig_len = snprintf(sig_text, sizeof(sig_text),
                                 "%u\n%s\nproducts/%s/devices/%s\n%s",
                                 (unsigned)expire_time, sig_method_name(method),
                                 product_id, device_name, ONENET_TOKEN_VERSION);
    if (sig_len < 0 || sig_len >= (int)sizeof(sig_text)) {
        ESP_LOGE(TAG, "待签字符串超长（需要 %d 字节，缓冲 %d）", sig_len, (int)sizeof(sig_text));
        return ESP_ERR_INVALID_SIZE;
    }

    /* ---- 第 2 步：Base64 解码设备密钥 ---- */
    unsigned char key[DEV_KEY_MAX];
    size_t key_len = 0;
    int rc = mbedtls_base64_decode(key, sizeof(key), &key_len,
                                   (const unsigned char *)device_key, strlen(device_key));
    if (rc != 0) {
        /* 最常见的成因：控制台里复制设备密钥时漏了尾巴或混进了空格。 */
        ESP_LOGE(TAG, "设备密钥不是合法 Base64（mbedtls rc=%d，长度 %u）",
                 rc, (unsigned)strlen(device_key));
        return ESP_FAIL;
    }

    /* ---- 第 3 步：HMAC 签名 ---- */
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(sig_md_type(method));
    if (md_info == NULL) {
        ESP_LOGE(TAG, "mbedTLS 里找不到所选摘要算法");
        memset(key, 0, sizeof(key));
        return ESP_ERR_NOT_SUPPORTED;
    }

    unsigned char mac[32];
    rc = mbedtls_md_hmac(md_info, key, key_len,
                         (const unsigned char *)sig_text, (size_t)sig_len, mac);
    /* 密钥用完立刻抹掉，尽量少在内存里留一份明文密钥 */
    memset(key, 0, sizeof(key));
    if (rc != 0) {
        ESP_LOGE(TAG, "HMAC 计算失败（mbedtls rc=%d）", rc);
        return ESP_FAIL;
    }

    /* ---- 第 4 步：Base64 编码签名（不带换行） ---- */
    unsigned char sign_b64[96];
    size_t sign_b64_len = 0;
    rc = mbedtls_base64_encode(sign_b64, sizeof(sign_b64) - 1, &sign_b64_len,
                               mac, (size_t)mbedtls_md_get_size(md_info));
    if (rc != 0) {
        ESP_LOGE(TAG, "签名 Base64 编码失败（mbedtls rc=%d）", rc);
        return ESP_FAIL;
    }
    sign_b64[sign_b64_len] = '\0';   /* mbedtls_base64_encode 不补结束符，自己补 */

    /* ---- 第 5 步：拼 token，sign 部分逐字节 URL 编码 ---- */
    const int head_len = snprintf(token, token_size,
                                  "version=%s&res=products%%2F%s%%2Fdevices%%2F%s"
                                  "&et=%u&method=%s&sign=",
                                  ONENET_TOKEN_VERSION, product_id, device_name,
                                  (unsigned)expire_time, sig_method_name(method));
    if (head_len < 0 || (size_t)head_len >= token_size) {
        ESP_LOGE(TAG, "token 固定部分就超出缓冲（需要 %d 字节，缓冲 %u）",
                 head_len, (unsigned)token_size);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t used = (size_t)head_len;
    for (size_t i = 0; i < sign_b64_len; i++) {
        /* 单个字符最坏展开成 3 字节，再加结束符 —— 3+1 是保守但简单的判断 */
        if (used + 3 >= token_size) {
            ESP_LOGE(TAG, "token 缓冲不足（已用 %u，缓冲 %u）",
                     (unsigned)used, (unsigned)token_size);
            token[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        used += url_encode_byte(token + used, sign_b64[i]);
    }
    token[used] = '\0';

    return ESP_OK;
}
