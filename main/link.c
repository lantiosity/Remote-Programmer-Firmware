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
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_sessions[LINK_SINK_COUNT] = {0, -1, -1};

void link_session_set(link_sink_t sink, int token)
{
    if (sink <= LINK_SINK_USB || sink >= LINK_SINK_COUNT) return;
    portENTER_CRITICAL(&s_state_lock);
    s_sessions[sink] = token;
    portEXIT_CRITICAL(&s_state_lock);
}

static void session_snapshot(int out[LINK_SINK_COUNT])
{
    portENTER_CRITICAL(&s_state_lock);
    memcpy(out, s_sessions, sizeof(s_sessions));
    portEXIT_CRITICAL(&s_state_lock);
}

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
    int seen[LINK_SINK_COUNT] = {0, -1, -1};
    int owner = -1;

    for (;;) {
        link_req_t *req = NULL;
        (void)xQueueReceive(s_queue, &req, pdMS_TO_TICKS(100));
        int sessions[LINK_SINK_COUNT];
        session_snapshot(sessions);
        probe_lock();
        for (int sink = LINK_SINK_TCP; sink < LINK_SINK_COUNT; ++sink) {
            if (seen[sink] == sessions[sink]) continue;
            dap_server_select_session((unsigned)(sink - LINK_SINK_TCP));
            dap_server_reset_handles();
            if (owner == sink) { probe_disconnect(); owner = -1; }
            seen[sink] = sessions[sink];
        }
        probe_unlock();
        if (!req) continue;
        if (req->sink != LINK_SINK_USB && sessions[req->sink] != req->opaque) {
            free(req);
            continue;
        }
        link_send_fn_t send = s_senders[req->sink];
        if (send == NULL) {
            ESP_LOGW(TAG, "sink %d 没有注册发送函数，丢弃请求", (int)req->sink);
        }
        if (send != NULL) {
            if (req->sink == LINK_SINK_USB) {
                uint8_t resp[DAP_PACKET_SIZE];
                /* Vendor OTA/config waits are unrelated to the SWD bus. */
                bool uses_probe = req->data[0] != 0x80;
                if (uses_probe) { probe_lock(); owner = LINK_SINK_USB; }
                uint32_t n = dap_cmsis_process(req->data, (uint32_t)req->len, resp, sizeof(resp));
                if (uses_probe) probe_unlock();
                if (n > 0) {
                    send(req->opaque, resp, n);
                }
            } else {
                probe_lock();
                owner = req->sink;
                dap_server_select_session((unsigned)(req->sink - LINK_SINK_TCP));
                size_t n = dap_server_process((const char *)req->data, req->len,
                                              s_json_resp, sizeof(s_json_resp));
                probe_unlock();
                if (n > 0) {
                    if (!send(req->opaque, (const uint8_t *)s_json_resp, n)) {
                        ESP_LOGW(TAG, "send failed on sink %d", (int)req->sink);
                    }
                }
            }
        }
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
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t link_submit(link_sink_t sink, int opaque, const uint8_t *data, size_t len)
{
    if (!s_started || data == NULL || len == 0 || sink < 0 || sink >= LINK_SINK_COUNT ||
        len > (sink == LINK_SINK_USB ? DAP_PACKET_SIZE : DAP_JSON_BUF - 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    link_req_t *req = malloc(sizeof(link_req_t) + len + 1);
    if (req == NULL) {
        link_note_drop();
        return ESP_ERR_NO_MEM;
    }
    req->sink = sink;
    req->opaque = opaque;
    req->len = len;
    memcpy(req->data, data, len);
    req->data[len] = '\0';

    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        free(req);
        link_note_drop();
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_state_lock);
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
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

void link_note_drop(void)
{
    portENTER_CRITICAL(&s_state_lock);
    ++s_stats.dropped;
    portEXIT_CRITICAL(&s_state_lock);
}

void link_get_stats(link_stats_t *out)
{
    if (out) {
        portENTER_CRITICAL(&s_state_lock);
        *out = s_stats;
        portEXIT_CRITICAL(&s_state_lock);
    }
}
