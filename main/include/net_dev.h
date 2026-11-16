/**
 * @file  net_dev.h
 * @brief  WiFi 链路：STA/AP + DAP TCP 服务(pyOCD remote 协议) + 串口 TCP 桥。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t net_dev_init(void);

/** 当前 IP（STA 或 AP），未联网返回 "0.0.0.0" */
const char *net_dev_ip(void);
const char *net_dev_ap_ip(void);
bool net_dev_sta_connected(void);
const char *net_dev_ssid(void);
unsigned net_dev_ap_clients(void);
unsigned net_dev_sta_retry_seconds(void);

/** 供 link 层回调：发送 JSON 行（自动补 '\n'） */
bool tcp_dap_send(int fd, const uint8_t *data, size_t len);

/** 供串口桥回调：发送原始字节 */
bool tcp_serial_send(int fd, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
