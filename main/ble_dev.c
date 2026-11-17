/**
 * @file  ble_dev.c
 * @brief  BLE 链路（NimBLE）：DAP JSON 桥 + NUS 虚拟串口。
 */
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "app_cfg.h"
#include "ble_dev.h"
#include "ble_ota.h"
#include "wifi_setup.h"
#include "dap_server.h"
#include "link.h"
#include "serial_bridge.h"
static const char *TAG = "ble";

/* 特征用途标记 */
enum {
    CH_DAP_RX = 1,
    CH_DAP_TX = 2,
    CH_NUS_RX = 3,
    CH_NUS_TX = 4,
};

/* UUID（按人类可读顺序书写，运行时转成小端） */
static const uint8_t UUID_DAP_SVC[16] = {0x6E, 0x40, 0x00, 0x10, 0xB5, 0xA3, 0xF3, 0x93,
                                         0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E};
static const uint8_t UUID_DAP_RX[16]  = {0x6E, 0x40, 0x00, 0x11, 0xB5, 0xA3, 0xF3, 0x93,
                                         0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E};
static const uint8_t UUID_DAP_TX[16]  = {0x6E, 0x40, 0x00, 0x12, 0xB5, 0xA3, 0xF3, 0x93,
                                         0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E};
static const uint8_t UUID_NUS_SVC[16] = {0x6E, 0x40, 0x00, 0x01, 0xB5, 0xA3, 0xF3, 0x93,
                                         0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E};
static const uint8_t UUID_NUS_RX[16]  = {0x6E, 0x40, 0x00, 0x02, 0xB5, 0xA3, 0xF3, 0x93,
                                         0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E};
static const uint8_t UUID_NUS_TX[16]  = {0x6E, 0x40, 0x00, 0x03, 0xB5, 0xA3, 0xF3, 0x93,
                                         0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E};

static ble_uuid128_t s_dap_svc, s_dap_rx, s_dap_tx;
static ble_uuid128_t s_nus_svc, s_nus_rx, s_nus_tx;

static uint16_t s_dap_tx_handle;
static uint16_t s_nus_tx_handle;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type;
static atomic_bool s_ready;
static atomic_bool s_dap_notify, s_nus_notify;

static uint8_t s_rx[DAP_JSON_BUF];
static size_t  s_rx_len;
static bool s_rx_failed;
static atomic_int s_token = ATOMIC_VAR_INIT(-1);
static uint16_t s_epoch;

static void uuid128_init(ble_uuid128_t *u, const uint8_t be[16])
{
    for (int i = 0; i < 16; ++i) {
        u->value[i] = be[15 - i];
    }
    u->u.type = BLE_UUID_TYPE_128;
}

/* ---------------- 收发 ---------------- */

/* Producers transfer ownership of each immutable packet to the queue. The
 * NimBLE host sends ONE fragment per event; retries are timer-driven, never
 * vTaskDelay() in the host. Generation tokens reject stale connection handles. */
typedef struct {
    int token;
    uint16_t handle;
    size_t len, offset;
    unsigned retries;
    uint8_t data[];
} notify_packet_t;
static QueueHandle_t s_tx_queue;
static struct ble_npl_event s_tx_ev;
static struct ble_npl_callout s_tx_retry;
static notify_packet_t *s_active;
static atomic_int s_overflow = ATOMIC_VAR_INIT(-1);

static void ble_tx_worker(struct ble_npl_event *ev)
{
    (void)ev;
    int token = atomic_load(&s_token);
    if (atomic_exchange(&s_overflow, -1) == token && token >= 0) {
        (void)ble_gap_terminate((uint16_t)token, BLE_ERR_REM_USER_CONN_TERM);
    }
    while (!s_active || s_active->token != token) {
        free(s_active);
        s_active = NULL;
        if (xQueueReceive(s_tx_queue, &s_active, 0) != pdTRUE) return;
    }
    if (s_active->handle == s_nus_tx_handle && !atomic_load(&s_nus_notify)) {
        free(s_active);
        s_active = NULL;
        (void)ble_npl_callout_reset(&s_tx_retry, ble_npl_time_ms_to_ticks32(1));
        return;
    }
    uint16_t conn = (uint16_t)token;
    uint16_t mtu = ble_att_mtu(conn);
    size_t chunk = mtu > 3 ? mtu - 3 : 20;
    if (chunk > 240) chunk = 240;
    size_t n = s_active->len - s_active->offset;
    if (n > chunk) n = chunk;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_active->data + s_active->offset, (uint16_t)n);
    int rc = om ? ble_gatts_notify_custom(conn, s_active->handle, om) : BLE_HS_ENOMEM;
    /* NimBLE consumes om on both success and failure. */
    if (!rc) {
        s_active->offset += n;
        s_active->retries = 0;
    } else if (rc != BLE_HS_ENOMEM || ++s_active->retries > 200) {
        ESP_LOGW(TAG, "BLE notify failed rc=%d; closing incomplete stream", rc);
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        free(s_active);
        s_active = NULL;
        return;
    }
    if (s_active->offset == s_active->len) { free(s_active); s_active = NULL; }
    (void)ble_npl_callout_reset(&s_tx_retry, ble_npl_time_ms_to_ticks32(1));
}

static bool notify_enqueue(int token, uint16_t handle, const uint8_t *data, size_t len, bool dap)
{
    if (!s_ready || token < 0 || token != atomic_load(&s_token) || !handle ||
        !data || !len || len > DAP_JSON_BUF || !s_tx_queue) return false;
    if (!(dap ? atomic_load(&s_dap_notify) : atomic_load(&s_nus_notify))) return false;
    notify_packet_t *packet = malloc(sizeof(*packet) + len + (dap ? 1 : 0));
    if (!packet) {
        link_note_drop();
        if (dap) {
            atomic_store(&s_overflow, token);
            ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
        }
        return false;
    }
    *packet = (notify_packet_t){.token=token, .handle=handle, .len=len + (dap ? 1 : 0)};
    memcpy(packet->data, data, len);
    if (dap) packet->data[len] = '\n';
    if (xQueueSend(s_tx_queue, &packet, dap ? pdMS_TO_TICKS(200) : 0) != pdTRUE) {
        free(packet);
        link_note_drop();
        if (dap) {
            atomic_store(&s_overflow, token);
            ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
        }
        return false;
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
    return true;
}

bool ble_dap_send(int token, const uint8_t *data, size_t len)
{
    return notify_enqueue(token, s_dap_tx_handle, data, len, true);
}

bool ble_serial_send(int token, const uint8_t *data, size_t len)
{
    return notify_enqueue(token, s_nus_tx_handle, data, len, false);
}

static bool ble_serial_sink(int opaque, const uint8_t *data, size_t len)
{
    if (ble_ota_in_progress()) return false;
    return ble_serial_send(opaque, data, len);
}

static void dap_input(const uint8_t *data, size_t len)
{
    if (s_rx_failed) return;
    if (len >= sizeof(s_rx) || s_rx_len + len >= sizeof(s_rx)) {
        s_rx_failed = true;
        s_rx_len = 0;
        atomic_store(&s_overflow, atomic_load(&s_token));
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
        return;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;

    size_t start = 0;
    for (size_t i = 0; i < s_rx_len; ++i) {
        if (s_rx[i] == '\n') {
            size_t l = i - start;
            while (l > 0 && s_rx[start + l - 1] == '\r') {
                --l;
            }
            if (l > 0) {
                if (link_submit(LINK_SINK_BLE, atomic_load(&s_token), s_rx + start, l) != ESP_OK) {
                    atomic_store(&s_overflow, atomic_load(&s_token));
                    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
                }
            }
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(s_rx, s_rx + start, s_rx_len - start);
        s_rx_len -= start;
    }
}

/* ---------------- GATT ---------------- */

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (conn_handle != s_conn) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    (void)attr_handle;
    const int kind = (int)(intptr_t)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (ble_ota_in_progress()) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t tmp[512];
        if (total > sizeof(tmp)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (os_mbuf_copydata(ctxt->om, 0, total, tmp) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (kind == CH_DAP_RX) {
            dap_input(tmp, total);
        } else if (kind == CH_NUS_RX) {
            serial_bridge_to_target(tmp, total);
        }
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t zero = 0;
        return (os_mbuf_append(ctxt->om, &zero, 1) == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_dap_chrs[] = {
    {
        .uuid = &s_dap_rx.u,
        .access_cb = gatt_access,
        .arg = (void *)(intptr_t)CH_DAP_RX,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_dap_tx.u,
        .access_cb = gatt_access,
        .arg = (void *)(intptr_t)CH_DAP_TX,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_dap_tx_handle,
    },
    {0},
};

static const struct ble_gatt_chr_def s_nus_chrs[] = {
    {
        .uuid = &s_nus_rx.u,
        .access_cb = gatt_access,
        .arg = (void *)(intptr_t)CH_NUS_RX,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_nus_tx.u,
        .access_cb = gatt_access,
        .arg = (void *)(intptr_t)CH_NUS_TX,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_nus_tx_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_dap_svc.u,
        .characteristics = s_dap_chrs,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_svc.u,
        .characteristics = s_nus_chrs,
    },
    {0},
};

/* ---------------- GAP ---------------- */

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
                (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
            s_conn = event->connect.conn_handle;
            s_dap_notify = false;
            s_nus_notify = false;
            int token = (int)s_conn | (int)((++s_epoch & 0x7fffu) << 16);
            atomic_store(&s_token, token);
            link_session_set(LINK_SINK_BLE, token);
            s_rx_len = 0;
            s_rx_failed = false;
            serial_bridge_subscribe(ble_serial_sink, atomic_load(&s_token));
            ESP_LOGI(TAG, "BLE 已连接 (conn=%d)", s_conn);
        } else {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE 断开 (reason=%d)", event->disconnect.reason);
        ble_ota_disconnected(event->disconnect.conn.conn_handle);
        wifi_setup_disconnected(event->disconnect.conn.conn_handle);
        if (event->disconnect.conn.conn_handle != s_conn) return 0;
        serial_bridge_unsubscribe(ble_serial_sink, atomic_load(&s_token));
        atomic_store(&s_token, -1);
        s_dap_notify = false;
        s_nus_notify = false;
        link_session_set(LINK_SINK_BLE, -1);
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_rx_len = 0;
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU = %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == s_conn) {
            if (event->subscribe.attr_handle == s_dap_tx_handle) s_dap_notify = event->subscribe.cur_notify;
            if (event->subscribe.attr_handle == s_nus_tx_handle) s_nus_notify = event->subscribe.cur_notify;
        }
        return 0;

    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = cfg_ble_name();
    fields.name = (uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败 rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "启动广播失败 rc=%d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr 失败 rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto 失败 rc=%d", rc);
        return;
    }
    advertise();
    ESP_LOGI(TAG, "BLE 广播已启动，设备名 %s", cfg_ble_name());
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_dev_init(void)
{
#ifdef CONFIG_LINK_BLE_ENABLE
    /* Per-notification INFO logging throttles OTA and crowds out the console. */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    uuid128_init(&s_dap_svc, UUID_DAP_SVC);
    uuid128_init(&s_dap_rx, UUID_DAP_RX);
    uuid128_init(&s_dap_tx, UUID_DAP_TX);
    uuid128_init(&s_nus_svc, UUID_NUS_SVC);
    uuid128_init(&s_nus_rx, UUID_NUS_RX);
    uuid128_init(&s_nus_tx, UUID_NUS_TX);

    esp_err_t nimble_err = nimble_port_init();
    if (nimble_err != ESP_OK) return nimble_err;

    esp_err_t ota_err = ble_ota_init();
    if (ota_err != ESP_OK) return ota_err;
    ble_ota_enable_notifications();

    ble_npl_event_init(&s_tx_ev, ble_tx_worker, NULL);
    s_tx_queue = xQueueCreate(8, sizeof(notify_packet_t *));
    if (!s_tx_queue) return ESP_ERR_NO_MEM;
    ble_npl_callout_init(&s_tx_retry, nimble_port_get_dflt_eventq(), ble_tx_worker, NULL);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_sc = 1;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg 失败 rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs 失败 rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_count_cfg(ble_ota_services());
    if (rc == 0) rc = ble_gatts_add_svcs(ble_ota_services());
    if (rc != 0) {
        ESP_LOGE(TAG, "OTA GATT registration failed rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_count_cfg(wifi_setup_services());
    if (rc == 0) rc = ble_gatts_add_svcs(wifi_setup_services());
    if (rc != 0) return ESP_FAIL;
    ble_svc_gap_device_name_set(cfg_ble_name());
    link_register_sender(LINK_SINK_BLE, ble_dap_send);
    nimble_port_freertos_init(host_task);
    s_ready = true;
    return ESP_OK;
#else
    ESP_LOGW(TAG, "BLE 已在 menuconfig 中关闭");
    return ESP_OK;
#endif
}

bool ble_dev_ready(void)
{
    return s_ready;
}
