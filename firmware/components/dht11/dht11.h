/**
 * @file dht11.h
 * @brief DHT11 单总线温湿度驱动 —— 为「每 10 分钟唤醒一次」的低功耗节点重写。
 *
 * ============================================================================
 * 本组件与参考工程 AloT\components\DHT11 的区别（有意为之，见 docs/PLAN.md）：
 *
 *   AloT 版本（常供电设备思路）          本组件（深睡节点思路）
 *   ---------------------------------   ------------------------------------
 *   dht11_start_task() 起一个常驻任务，   没有任务、没有全局状态、没有任何周期性活动。
 *   每 2 秒轮询一次 + 5 点滑动平均        一次唤醒只调用一次 dht11_read_once()。
 *   没有传感器电源控制                   调用前给传感器上电，返回前一定断电。
 *   数据线一直挂着内部上拉               返回前把内部上拉拉掉（不变量 ④）。
 *   失败只打印日志，返回值给谁用不确定    返回值明确；失败时**不写**输出参数。
 *
 * 三条设计不变量（PLAN.md 2.6 节）在本文件里的体现：
 *   ① 任何路径都必须最终进入深度睡眠  → 本组件不阻塞、不循环、不等待外部事件。
 *   ② 所有阻塞调用必须有上界超时      → 每个等待都有 CLK 超时，重读只重一次。
 *   ④ 睡眠前传感器必须断电、数据线不再被内部上拉拉高
 *                                    → dht11_read_once() 无论成功失败，
 *                                      返回前都执行 dht11_safe_off()。
 * ============================================================================
 */

#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 时序参数
 *   这些数字属于「DHT11 这个协议」而不是「本应用」，所以放在驱动头文件里，
 *   不放进 main/app_config.h（那个文件管的是应用级参数）。
 *   需要实验时可以用编译选项 -D 覆盖，例如 -DDHT11_POWER_SETTLE_MS=2000。
 * =========================================================================== */

/** 起始信号拉低时长（ms）。DHT11 要求 ≥18ms。 */
#ifndef DHT11_START_LOW_MS
#define DHT11_START_LOW_MS          20
#endif

/** 上电后等待稳定（ms）。DHT11 上电后约 1s 内读数无效，项目书 6.4 节取 1.2s。 */
#ifndef DHT11_POWER_SETTLE_MS
#define DHT11_POWER_SETTLE_MS       1200
#endif

/** 第一次读失败后的重读等待（ms）。DHT11 采样率上限 1Hz，必须 >2s。 */
#ifndef DHT11_RETRY_DELAY_MS
#define DHT11_RETRY_DELAY_MS        2100
#endif

/** 单个电平阶段的超时（µs）。超过即认为时序异常，放弃本次读取。 */
#ifndef DHT11_BIT_TIMEOUT_US
#define DHT11_BIT_TIMEOUT_US        100
#endif

/** 判定「数据位 = 1」的高电平下限（µs）。DHT11 的 0 约 26~28µs、1 约 70µs。 */
#ifndef DHT11_BIT_ONE_MIN_US
#define DHT11_BIT_ONE_MIN_US        40
#endif

/* ===========================================================================
 * API
 * =========================================================================== */

/**
 * @brief 一次性完整读取温湿度：上电 → 等稳定 → 读 →（失败则重读一次）→ 断电 → 释放数据线。
 *
 * @param data_pin   DHT11 的 DATA 脚（本项目 = GPIO4）。
 * @param power_pin  DHT11 的 VCC 门控脚。
 *                   **传 -1 表示传感器接的是常供 3.3V**，不做电源门控。
 * @param[out] temperature 温度（摄氏度）。仅在返回 ESP_OK 时被写入。
 * @param[out] humidity    相对湿度（%RH）。仅在返回 ESP_OK 时被写入。
 *
 * @retval ESP_OK                    两次尝试内读到了通过校验且在合理范围内的值。
 * @retval ESP_ERR_INVALID_ARG       参数为空 / 引脚号非法。
 * @retval ESP_ERR_TIMEOUT           传感器没有响应（最常见原因：没接、没上拉、没供电）。
 * @retval ESP_ERR_INVALID_RESPONSE  收到全 0，或读数超出物理合理范围。
 * @retval ESP_ERR_INVALID_CRC       校验和不对（时序被抢占，重读通常能救回来）。
 *
 * @note 本函数**一定会**在返回前把传感器断电并把数据线设为「输入 + 内部上拉关闭」，
 *       所以无论返回什么错误，调用方都可以直接进深度睡眠。
 * @note 本函数是阻塞的，最坏耗时 = 1200 + 5 + 2100 + 5 ≈ 3.3 秒。
 */
esp_err_t dht11_read_once(gpio_num_t data_pin, gpio_num_t power_pin,
                          float *temperature, float *humidity);

/**
 * @brief 把数据线设为「输入 + 内部上拉关闭」。
 *
 * 为什么必须关内部上拉：传感器断电后，如果本芯片还在往 DATA 线上拉高，
 * 电流会经模块上拉电阻 + DHT11 内部保护二极管倒灌进传感器的 VCC 轨，
 * 把它「幽灵供电」到一个不高不低的电压 —— 既漏电又可能让传感器半死不活。
 */
void dht11_release_data_pin(gpio_num_t data_pin);

/**
 * @brief 断电 + 释放数据线。任何错误路径上都可以安全地重复调用。
 * @param power_pin 传 -1 表示不做电源门控（只释放数据线）。
 */
void dht11_safe_off(gpio_num_t data_pin, gpio_num_t power_pin);

#ifdef __cplusplus
}
#endif
