/**
 * @file  link.h
 * @brief  链路分发：把各传输通道（USB HID / TCP / BLE）的请求串行化，
 *         交给 DAP 处理线程，并按来源回送响应。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LINK_SINK_USB = 0,
    LINK_SINK_TCP = 1,
    LINK_SINK_BLE = 2,
    LINK_SINK_COUNT
} link_sink_t;

/** 发送函数：opaque 为该链路的句柄（TCP fd / BLE conn handle），USB 忽略 */
typedef bool (*link_send_fn_t)(int opaque, const uint8_t *data, size_t len);

/** 注册某条链路的发送实现 */
void link_register_sender(link_sink_t sink, link_send_fn_t fn);

esp_err_t link_init(void);

/** 提交一条请求（内部拷贝数据） */
esp_err_t link_submit(link_sink_t sink, int opaque, const uint8_t *data, size_t len);

/** 统计信息 */
typedef struct {
    uint32_t usb_req;
    uint32_t tcp_req;
    uint32_t ble_req;
    uint32_t dropped;
    uint32_t last_req_len;
    uint32_t max_req_len;
} link_stats_t;

/** 由传输层（如 HID 发送失败）上报丢包 */
void link_note_drop(void);

void link_get_stats(link_stats_t *out);

#ifdef __cplusplus
}
#endif
