/* Shared BLE / USB / WiFi self-update. Flash operations run in a worker, never in NimBLE callbacks.
 * Protocol v1: control BEGIN(1,size32,sha256), FINISH(3), ACTIVATE(4), ABORT(5).
 * Data: offset32 + up to 480 bytes. All integers are little-endian.
 */
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "psa/crypto.h"
#include "app_cfg.h"
#include "ble_ota.h"
#include "net_dev.h"
#include "web_config.h"
#include "ota_policy.h"

enum { IDLE, ARMED, PREPARING, RECEIVING, VERIFYING, READY, REBOOTING, ERROR };
enum { BEGIN = 1, DATA, FINISH, ACTIVATE, ABORT };
enum { ERR_PROTOCOL = 0x7001, ERR_LOCKED, ERR_OFFSET, ERR_HASH,
       ERR_TIMEOUT, ERR_DISCONNECTED, ERR_SIZE, ERR_PROJECT };
typedef struct {
    uint16_t conn, length;
    uint32_t epoch;
    uint8_t op, payload[OTA_MAX_PAYLOAD];
} ota_job_t;

static const char *TAG = "ble_ota";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_jobs;
static struct ble_npl_event s_notify_event;
static bool s_notifications_ready;
static uint16_t s_status_handle, s_owner = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_state, s_op;
static uint32_t s_received, s_total, s_error, s_session, s_epoch;
static bool s_busy, s_cancel, s_reboot_reserved;
static int64_t s_armed_until, s_last_activity;
static unsigned s_armed_transports;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_partition;
static psa_hash_operation_t s_hash = PSA_HASH_OPERATION_INIT;
static uint8_t s_expected[32];
static bool s_boot_healthy;
static int64_t s_confirm_at;

/* 6e400020/21/22/23/24-b5a3-f393-e0a9-e50e24dcca9e */
#define OTA_UUID(n) BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0, \
                                   0x93,0xf3,0xa3,0xb5,n,0x00,0x40,0x6e)
static const ble_uuid128_t s_service_uuid = OTA_UUID(0x20);
static const ble_uuid128_t s_control_uuid = OTA_UUID(0x21);
static const ble_uuid128_t s_data_uuid = OTA_UUID(0x22);
static const ble_uuid128_t s_status_uuid = OTA_UUID(0x23);
static const ble_uuid128_t s_info_uuid = OTA_UUID(0x24);

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static unsigned owner_transport(uint16_t owner)
{
    return owner == OTA_OWNER_USB ? OTA_ALLOW_USB :
        owner == OTA_OWNER_WIFI ? OTA_ALLOW_WIFI : OTA_ALLOW_BLE;
}

static void status_for_transport(unsigned transport, uint8_t out[20])
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool armed = ota_window_open(now, s_armed_until, s_armed_transports, transport);
    out[0] = 1;
    out[1] = s_state == ARMED && !armed ? IDLE : s_state;
    out[2] = s_op;
    out[3] = (armed ? 1 : 0) | ((s_busy || s_cancel || s_reboot_reserved) ? 2 : 0);
    put32(out + 4, s_received);
    put32(out + 8, s_total);
    put32(out + 12, s_error);
    put32(out + 16, s_session);
    portEXIT_CRITICAL(&s_lock);
}

void ble_ota_status(uint8_t out[20])
{
    status_for_transport(OTA_ALLOW_ALL, out);
}

void ble_ota_status_for(uint16_t owner, uint8_t out[20])
{
    status_for_transport(owner_transport(owner), out);
}

static bool ota_active(void)
{
    return s_busy || s_cancel || (s_state >= PREPARING && s_state <= REBOOTING);
}

bool ble_ota_in_progress(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = ota_active();
    portEXIT_CRITICAL(&s_lock);
    return active;
}

esp_err_t ble_ota_reserve_reboot(void)
{
    portENTER_CRITICAL(&s_lock);
    bool available = !s_reboot_reserved && !ota_active();
    if (available) s_reboot_reserved = true;
    portEXIT_CRITICAL(&s_lock);
    return available ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void ble_ota_release_reboot(void)
{
    portENTER_CRITICAL(&s_lock);
    s_reboot_reserved = false;
    portEXIT_CRITICAL(&s_lock);
}

size_t ble_ota_info(char *out, size_t capacity)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    const esp_partition_t *offline = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY, "offline");
    char elf[65];
    /* esp_app_get_elf_sha256() may be configured to return only 9 characters. */
    for (unsigned i = 0; i < 32; ++i) snprintf(elf + 2*i, 3, "%02x", app->app_elf_sha256[i]);
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) (void)esp_ota_get_state_partition(running, &state);
    int n = snprintf(out, capacity,
        "{\"protocol\":1,\"wifi_setup\":1,\"serial\":\"%s\",\"project\":\"%s\",\"version\":\"%s\","
        "\"running\":\"%s\",\"boot\":\"%s\",\"slot_size\":%lu,\"image_state\":%u,"
        "\"elf_sha256\":\"%s\",\"layout\":%u,\"factory_size\":%lu,\"offline_size\":%lu,"
        "\"ota_port\":%u,\"wifi_ip\":\"%s\",\"wifi_sta\":%s,\"ap_clients\":%u,\"sta_retry_s\":%u}",
        cfg_serial_number(), app->project_name, app->version,
        running ? running->label : "", boot ? boot->label : "",
        (unsigned long)(next ? next->size : 0), (unsigned)state, elf, offline ? 2 : 1,
        (unsigned long)(factory ? factory->size : 0), (unsigned long)(offline ? offline->size : 0),
        (unsigned)CONFIG_LINK_OTA_TCP_PORT, net_dev_ip(), net_dev_sta_connected() ? "true" : "false",
        net_dev_ap_clients(), net_dev_sta_retry_seconds());
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0;
}

/* This event runs on the NimBLE host. Notifications are a hint; status reads
 * always expose the latest committed offset if a notification is lost. */
static void notify_worker(struct ble_npl_event *event)
{
    (void)event;
    portENTER_CRITICAL(&s_lock);
    uint16_t conn = s_owner;
    portEXIT_CRITICAL(&s_lock);
    if (conn >= OTA_OWNER_USB) return;
    uint8_t status[20];
    ble_ota_status_for(conn, status);
    static uint32_t requested_session;
    uint32_t session = get32(status + 16);
    if (status[1] == RECEIVING && status[2] == BEGIN && session != requested_session) {
        const struct ble_gap_upd_params params = {
            .itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 400,
        }; /* Request 7.5..15ms instead of a slow peripheral default. */
        (void)ble_gap_update_params(conn, &params);
        requested_session = session;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, sizeof(status));
    if (om) (void)ble_gatts_notify_custom(conn, s_status_handle, om); /* consumes om */
}

static void publish(uint8_t state, uint8_t op, uint32_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_state = state;
    s_op = op;
    s_error = error;
    s_busy = false;
    bool notify = s_notifications_ready && s_owner < OTA_OWNER_USB;
    portEXIT_CRITICAL(&s_lock);
    if (notify)
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_notify_event);
}

static void discard(void)
{
    if (s_handle) {
        (void)esp_ota_abort(s_handle);
        s_handle = 0;
    }
    (void)psa_hash_abort(&s_hash);
    s_partition = NULL;
}

static void fail(uint8_t op, uint32_t error)
{
    discard();
    publish(ERROR, op, error);
}

esp_err_t ble_ota_arm(uint32_t seconds)
{
    return ble_ota_arm_transport(seconds, OTA_ALLOW_ALL);
}

bool ble_ota_window_for(unsigned transport)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool allowed = ota_window_open(now, s_armed_until, s_armed_transports, transport);
    portEXIT_CRITICAL(&s_lock);
    return allowed;
}

esp_err_t ble_ota_arm_transport(uint32_t seconds, unsigned transports)
{
    if (!s_jobs) return ESP_ERR_INVALID_STATE;
    if (seconds > 600 || !transports || (transports & ~OTA_ALLOW_ALL)) return ESP_ERR_INVALID_ARG;
    int64_t deadline = esp_timer_get_time() + (int64_t)seconds * 1000000;
    portENTER_CRITICAL(&s_lock);
    if (s_reboot_reserved || ota_active()) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_armed_until = seconds ? deadline : 0;
    s_armed_transports = seconds ? transports : 0;
    s_state = seconds ? ARMED : IDLE;
    s_error = 0;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "OTA window: %lu seconds, transport mask=%u", (unsigned long)seconds, transports);
    return ESP_OK;
}

void ble_ota_disconnected(uint16_t conn)
{
    portENTER_CRITICAL(&s_lock);
    if (s_owner == conn && s_state != REBOOTING) {
        if (s_busy || (s_state >= PREPARING && s_state <= READY)) {
            ++s_epoch;
            s_cancel = true;
        } else {
            /* A clean ABORT or an already reported error has no live image to
             * cancel. Preserve its result when the client closes normally. */
            s_owner = BLE_HS_CONN_HANDLE_NONE;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static void process(const ota_job_t *job)
{
    portENTER_CRITICAL(&s_lock);
    uint8_t state = s_state;
    uint32_t received = s_received, total = s_total;
    portEXIT_CRITICAL(&s_lock);
    if (job->op == BEGIN) {
        discard();
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        uint32_t size = get32(job->payload);
        if (!part || size < 288 || size > part->size) { fail(BEGIN, ERR_SIZE); return; }
        portENTER_CRITICAL(&s_lock);
        s_total = size;
        s_received = 0;
        ++s_session;
        s_state = PREPARING;
        portEXIT_CRITICAL(&s_lock);
        memcpy(s_expected, job->payload + 4, 32);
        s_partition = part;
        esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
        if (err != ESP_OK) { fail(BEGIN, err); return; }
        if (psa_hash_setup(&s_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            fail(BEGIN, ESP_FAIL); return;
        }
        portENTER_CRITICAL(&s_lock);
        s_last_activity = esp_timer_get_time();
        portEXIT_CRITICAL(&s_lock);
        publish(RECEIVING, BEGIN, 0);
    } else if (job->op == DATA) {
        uint32_t offset = get32(job->payload);
        size_t size = job->length - 4;
        if (state != RECEIVING || offset != received) { fail(DATA, ERR_OFFSET); return; }
        if (received > total || size > total - received) { fail(DATA, ERR_SIZE); return; }
        esp_err_t err = esp_ota_write(s_handle, job->payload + 4, size);
        if (err != ESP_OK) { fail(DATA, err); return; }
        if (psa_hash_update(&s_hash, job->payload + 4, size) != PSA_SUCCESS) {
            fail(DATA, ESP_FAIL); return;
        }
        portENTER_CRITICAL(&s_lock);
        s_received += size;
        s_last_activity = esp_timer_get_time();
        portEXIT_CRITICAL(&s_lock);
        publish(RECEIVING, DATA, 0);
    } else if (job->op == FINISH) {
        if (state != RECEIVING || received != total) { fail(FINISH, ERR_SIZE); return; }
        portENTER_CRITICAL(&s_lock);
        s_state = VERIFYING;
        portEXIT_CRITICAL(&s_lock);
        uint8_t digest[32];
        size_t written = 0;
        if (psa_hash_finish(&s_hash, digest, sizeof(digest), &written) != PSA_SUCCESS ||
            written != 32 || memcmp(digest, s_expected, 32)) { fail(FINISH, ERR_HASH); return; }
        esp_err_t err = esp_ota_end(s_handle);
        s_handle = 0; /* esp_ota_end frees the handle even when validation fails */
        if (err != ESP_OK) { fail(FINISH, err); return; }
        esp_app_desc_t app;
        err = esp_ota_get_partition_description(s_partition, &app);
        if (err != ESP_OK || strncmp(app.project_name, esp_app_get_description()->project_name,
                                     sizeof(app.project_name))) { fail(FINISH, ERR_PROJECT); return; }
        portENTER_CRITICAL(&s_lock);
        s_last_activity = esp_timer_get_time();
        portEXIT_CRITICAL(&s_lock);
        publish(READY, FINISH, 0); /* Boot partition still unchanged. */
    } else if (job->op == ACTIVATE) {
        if (state != READY || !s_partition) { fail(ACTIVATE, ERR_PROTOCOL); return; }
        esp_err_t err = esp_ota_set_boot_partition(s_partition);
        if (err != ESP_OK) { fail(ACTIVATE, err); return; }
        publish(REBOOTING, ACTIVATE, 0);
        ESP_LOGI(TAG, "OTA verified, rebooting into %s", s_partition->label);
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
    } else if (job->op == ABORT) {
        discard();
        portENTER_CRITICAL(&s_lock);
        s_received = s_total = 0;
        portEXIT_CRITICAL(&s_lock);
        publish(IDLE, ABORT, 0);
    }
}

static int access_ota(uint16_t conn, uint16_t handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)handle;
    int kind = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t status[20];
        char info[512];
        if (kind == 3) {
            ble_ota_status_for(conn, status);
            return os_mbuf_append(ctxt->om, status, sizeof(status)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        }
        size_t n = ble_ota_info(info, sizeof(info));
        return n && !os_mbuf_append(ctxt->om, info, n) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || (kind != 1 && kind != 2))
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    ota_job_t job = {.conn = conn};
    if (length == 0 || length > 484) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (os_mbuf_copydata(ctxt->om, 0, length, job.payload)) return BLE_ATT_ERR_UNLIKELY;
    if (kind == 2) {
        if (length < 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        job.op = DATA;
        job.length = length;
    } else {
        job.op = job.payload[0];
        if (job.op == BEGIN) {
            if (length != 37) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            memmove(job.payload, job.payload + 1, 36);
            job.length = 36;
        } else if ((job.op != FINISH && job.op != ACTIVATE && job.op != ABORT) || length != 1)
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    esp_err_t err = ble_ota_submit(conn, job.op, job.payload, job.length);
    return err == ESP_OK ? 0 : err == ESP_ERR_NOT_ALLOWED ? BLE_ATT_ERR_WRITE_NOT_PERMITTED :
           err == ESP_ERR_INVALID_ARG ? BLE_ATT_ERR_VALUE_NOT_ALLOWED : BLE_ATT_ERR_INSUFFICIENT_RES;
}

esp_err_t ble_ota_submit(uint16_t owner, uint8_t op, const uint8_t *payload, size_t length)
{
    if (!s_jobs) return ESP_ERR_INVALID_STATE;
    if (owner == BLE_HS_CONN_HANDLE_NONE || length > OTA_MAX_PAYLOAD ||
        (length && !payload) ||
        (op == BEGIN ? length != 36 : op == DATA ? length < 5 :
         (op != FINISH && op != ACTIVATE && op != ABORT) || length != 0))
        return ESP_ERR_INVALID_ARG;
    ota_job_t job = {.conn = owner, .op = op, .length = length};
    if (length) memcpy(job.payload, payload, length);
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_reboot_reserved || s_busy || s_cancel || s_state == REBOOTING) {
        portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE;
    }
    bool active = s_state >= PREPARING && s_state <= READY;
    unsigned transport = owner_transport(owner);
    if ((active && s_owner != owner) ||
        (job.op == BEGIN && (active || !ota_window_open(now, s_armed_until, s_armed_transports, transport))) ||
        (job.op != BEGIN && (!active || s_owner != owner))) {
        portEXIT_CRITICAL(&s_lock); return ESP_ERR_NOT_ALLOWED;
    }
    if (job.op == BEGIN) s_owner = owner;
    job.epoch = s_epoch;
    s_busy = true;
    portEXIT_CRITICAL(&s_lock);
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock); s_busy = false; portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

#ifdef CONFIG_BLE_OTA_REQUIRE_ENCRYPTION
#define OTA_WRITE_FLAGS (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC)
#else
#define OTA_WRITE_FLAGS BLE_GATT_CHR_F_WRITE
#endif
static const struct ble_gatt_chr_def s_chars[] = {
    {.uuid=&s_control_uuid.u, .access_cb=access_ota, .arg=(void *)1, .flags=OTA_WRITE_FLAGS},
    {.uuid=&s_data_uuid.u, .access_cb=access_ota, .arg=(void *)2, .flags=OTA_WRITE_FLAGS},
    {.uuid=&s_status_uuid.u, .access_cb=access_ota, .arg=(void *)3,
     .flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY, .val_handle=&s_status_handle},
    {.uuid=&s_info_uuid.u, .access_cb=access_ota, .arg=(void *)4, .flags=BLE_GATT_CHR_F_READ},
    {0},
};
static const struct ble_gatt_svc_def s_services[] = {
    {.type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=&s_service_uuid.u, .characteristics=s_chars}, {0},
};
const struct ble_gatt_svc_def *ble_ota_services(void) { return s_services; }

void ble_ota_boot_confirm(bool healthy)
{
    portENTER_CRITICAL(&s_lock);
    s_boot_healthy = healthy;
    s_confirm_at = esp_timer_get_time() + 5000000;
    portEXIT_CRITICAL(&s_lock);
}

static void worker(void *arg)
{
    (void)arg;
    int64_t button_since = 0;
    bool button_fired = false;
    for (;;) {
        ota_job_t job;
        bool have_job = xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(100)) == pdTRUE;
        portENTER_CRITICAL(&s_lock);
        bool cancel = s_cancel;
        uint32_t epoch = s_epoch;
        s_cancel = false;
        if (cancel) s_busy = true;
        int64_t confirm_at = s_confirm_at;
        bool healthy = s_boot_healthy;
        portEXIT_CRITICAL(&s_lock);
        if (cancel) {
            discard();
            portENTER_CRITICAL(&s_lock); s_owner = BLE_HS_CONN_HANDLE_NONE; portEXIT_CRITICAL(&s_lock);
            publish(ERROR, ABORT, ERR_DISCONNECTED);
        }
        if (have_job && job.epoch == epoch && !cancel) process(&job);
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        int64_t timeout_us = (int64_t)(s_state == READY ? CONFIG_OTA_READY_TIMEOUT_SECONDS :
            CONFIG_OTA_IDLE_TIMEOUT_SECONDS) * 1000000;
        bool expired = !s_busy && !s_cancel && (s_state == RECEIVING || s_state == READY) &&
            now - s_last_activity > timeout_us;
        if (expired) s_busy = true; /* Reserve the timeout transition against new jobs. */
        portEXIT_CRITICAL(&s_lock);
        if (expired)
            fail(ABORT, ERR_TIMEOUT);
        if (confirm_at && now >= confirm_at) {
            portENTER_CRITICAL(&s_lock); s_confirm_at = 0; portEXIT_CRITICAL(&s_lock);
            esp_ota_img_states_t state;
            if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
                state == ESP_OTA_IMG_PENDING_VERIFY) {
                if (healthy) {
                    ESP_LOGI(TAG, "Startup checks passed; accepting OTA image");
                    (void)esp_ota_mark_app_valid_cancel_rollback();
                } else {
                    ESP_LOGE(TAG, "Startup checks failed; rolling back");
                    (void)esp_ota_mark_app_invalid_rollback_and_reboot();
                }
            }
        }
#if CONFIG_BLE_OTA_BUTTON_GPIO >= 0
        if (!gpio_get_level(CONFIG_BLE_OTA_BUTTON_GPIO)) {
            if (!button_since) button_since = now;
            if (!button_fired && now - button_since >= 2000000) {
                (void)ble_ota_arm(300);
                (void)web_config_arm();
                button_fired = true;
            }
        } else { button_since = 0; button_fired = false; }
#endif
    }
}

esp_err_t ble_ota_init(void)
{
    if (s_jobs) return ESP_OK;
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
#if CONFIG_BLE_OTA_BUTTON_GPIO >= 0
    const cfg_probe_pins_t *pins = cfg_probe();
    const int button_gpio = CONFIG_BLE_OTA_BUTTON_GPIO;
    if (!GPIO_IS_VALID_GPIO(button_gpio) || button_gpio == pins->swclk_gpio ||
        button_gpio == pins->swdio_gpio || button_gpio == pins->nreset_gpio ||
        button_gpio == cfg_uart_tx_gpio() || button_gpio == cfg_uart_rx_gpio())
        return ESP_ERR_INVALID_ARG;
    gpio_config_t button = {.pin_bit_mask=1ULL << CONFIG_BLE_OTA_BUTTON_GPIO,
        .mode=GPIO_MODE_INPUT, .pull_up_en=GPIO_PULLUP_ENABLE};
    esp_err_t err = gpio_config(&button);
    if (err != ESP_OK) return err;
#endif
    s_jobs = xQueueCreate(1, sizeof(ota_job_t));
    if (!s_jobs) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "ble_ota", 6144, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_jobs); s_jobs = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ble_ota_enable_notifications(void)
{
    ble_npl_event_init(&s_notify_event, notify_worker, NULL);
    portENTER_CRITICAL(&s_lock);
    s_notifications_ready = true;
    portEXIT_CRITICAL(&s_lock);
}
