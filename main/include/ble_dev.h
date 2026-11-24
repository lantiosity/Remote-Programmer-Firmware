/**
 * @file  ble_dev.h
 * @brief  BLE 链路：DAP 服务（自定义 128 位 UUID）+ NUS 虚拟串口。
 *
 * UUID 定义（与 tools/wirelink 中的 PC 端保持一致）：
 *   DAP 服务   6E400010-B5A3-F393-E0A9-E50E24DCCA9E
 *     RX(写)   6E400011-...   接收 JSON 行
 *     TX(通知) 6E400012-...   回送 JSON 行
 *   NUS 服务   6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *     RX(写)   6E400002-...   发给目标串口
 *     TX(通知) 6E400003-...   来自目标串口
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_dev_init(void);
bool      ble_dev_ready(void);

/** 发送 DAP 响应（自动补 '\n' 并分片） */
bool ble_dap_send(int conn, const uint8_t *data, size_t len);

/** 发送串口数据（NUS 通知，分片） */
bool ble_serial_send(int conn, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
