/**
 * @file  usb_dev.h
 * @brief  USB 复合设备：CMSIS-DAP HID + CDC 虚拟串口。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_dev_init(void);

/** HID（CMSIS-DAP）是否已挂载 */
bool usb_dev_hid_ready(void);

/** 发送一个 64 字节的 CMSIS-DAP 响应 */
void usb_dev_hid_send(const uint8_t *data, size_t len);

/** CDC 虚拟串口端点已就绪即可收发，不要求主机置位 DTR。 */
bool   usb_dev_cdc_connected(void);
size_t usb_dev_cdc_read(uint8_t *buf, size_t len);
void   usb_dev_cdc_write(const uint8_t *data, size_t len);
bool   usb_dev_cdc_dtr(void);
bool   usb_dev_cdc_rts(void);

#ifdef __cplusplus
}
#endif
