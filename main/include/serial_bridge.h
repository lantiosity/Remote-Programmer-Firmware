/**
 * @file  serial_bridge.h
 * @brief  目标串口的双向桥：目标 UART <-> USB CDC / BLE NUS / TCP。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*serial_sink_fn_t)(int opaque, const uint8_t *data, size_t len);

esp_err_t serial_bridge_init(void);

/** 启动收发任务（init 之后调用） */
void serial_bridge_start(void);

/** 向目标串口写数据 */
bool serial_bridge_to_target(const uint8_t *data, size_t len);

/** 订阅"来自目标串口"的数据（BLE / TCP 各订阅一个） */
esp_err_t serial_bridge_subscribe(serial_sink_fn_t fn, int opaque);
void      serial_bridge_unsubscribe(serial_sink_fn_t fn, int opaque);

/** 运行期修改波特率 */
esp_err_t serial_bridge_set_baud(int baud);
int       serial_bridge_get_baud(void);

#ifdef __cplusplus
}
#endif
