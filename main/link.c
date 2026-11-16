/**
 * @file  link.c
 * @brief  链路分发与 DAP 工作线程。
 *
 * 所有链路的请求都进入同一个队列，由一个高优先级线程串行执行（SWD 只有一套硬件，
 * 天然需要互斥），执行完按请求来源回送响应。
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "dap_cmsis.h"
#include "probe_core.h"
#include "dap_server.h"
#include "link.h"

static const char *TAG = "link";

#define LINK_QUEUE_DEPTH 8

typedef struct {
    link_sink_t sink;
    int         opaque;
    size_t      len;
    uint8_t     data[];
} link_req_t;

static QueueHandle_t    s_queue;
static link_send_fn_t   s_senders[LINK_SINK_COUNT];
static link_stats_t     s_stats;
static bool             s_started;

void link_register_sender(link_sink_t sink, link_send_fn_t fn)
{
    if (sink < LINK_SINK_COUNT) {
        s_senders[sink] = fn;
    }
}

static void link_worker(void *arg)
{
    static char s_json_resp[DAP_JSON_BUF];
    (void)arg;

    for (;;) {
        link_req_t *req = NULL;
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE || req == NULL) {
            continue;
        }
        probe_lock();
        link_send_fn_t send = s_senders[req->sink];
        if (send == NULL) {
            ESP_LOGW(TAG, "sink %d 没有注册发送函数，丢弃请求", (int)req->sink);
        }
        if (send != NULL) {
            if (req->sink == LINK_SINK_USB) {
                uint8_t resp[DAP_PACKET_SIZE];
                uint32_t n = dap_cmsis_process(req->data, (uint32_t)req->len, resp, sizeof(resp));
                if (n > 0) {
                    send(req->opaque, resp, n);
                }
            } else {
                size_t n = dap_server_process((const char *)req->data, req->len,
                                              s_json_resp, sizeof(s_json_resp));
                if (n > 0) {
                    if (!send(req->opaque, (const uint8_t *)s_json_resp, n)) {
                        ESP_LOGW(TAG, "send failed on sink %d", (int)req->sink);
                    }
                }
            }
        }
        probe_unlock();
        free(req);
    }
}

esp_err_t link_init(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(LINK_QUEUE_DEPTH, sizeof(link_req_t *));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(link_worker, "dap_worker", 8192, NULL,
                                            12, NULL, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t link_submit(link_sink_t sink, int opaque, const uint8_t *data, size_t len)
{
    if (!s_started || data == NULL || len == 0 || sink >= LINK_SINK_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    link_req_t *req = malloc(sizeof(link_req_t) + len + 1);
    if (req == NULL) {
        ++s_stats.dropped;
        return ESP_ERR_NO_MEM;
    }
    req->sink = sink;
    req->opaque = opaque;
    req->len = len;
    memcpy(req->data, data, len);
    req->data[len] = '\0';

    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        probe_unlock();
        free(req);
        ++s_stats.dropped;
        return ESP_ERR_TIMEOUT;
    }
    if (sink == LINK_SINK_USB) {
        ++s_stats.usb_req;
    } else if (sink == LINK_SINK_TCP) {
        ++s_stats.tcp_req;
    } else {
        ++s_stats.ble_req;
    }
    s_stats.last_req_len = (uint32_t)len;
    if (len > s_stats.max_req_len) {
        s_stats.max_req_len = (uint32_t)len;
    }
    return ESP_OK;
}

void link_note_drop(void)
{
    ++s_stats.dropped;
}

void link_get_stats(link_stats_t *out)
{
    if (out) {
        *out = s_stats;
    }
}
