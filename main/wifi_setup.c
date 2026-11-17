#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "app_cfg.h"
#include "ble_ota.h"
#include "net_dev.h"
#include "wifi_setup.h"

#define REQUEST_SIZE 224
#define REPLY_SIZE 512
typedef struct { char text[REQUEST_SIZE]; bool usb; } job_t;
static QueueHandle_t s_jobs;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
static char s_reply[REPLY_SIZE] = "{\"id\":\"\",\"ok\":false,\"error\":\"no_request\"}";
/* Scan cache/reply production belong to the single worker. BLE RX assembly
 * belongs only to NimBLE host callbacks; USB assembles its own request. */
static wifi_ap_record_t s_networks[20];
static uint16_t s_count;
static char s_rx[REQUEST_SIZE];
static size_t s_rx_len;
static uint16_t s_rx_conn = BLE_HS_CONN_HANDLE_NONE;

static void hex_encode(const char *in, char *out, size_t max)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < max && in[i]; ++i) { out[2*i] = digits[(unsigned char)in[i] >> 4]; out[2*i+1] = digits[in[i] & 15]; }
    out[2*i] = 0;
}
static bool hex_decode(const char *hex, char *out, size_t max)
{
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > max) return false;
    for (size_t i = 0; i < n / 2; ++i) {
        unsigned value = 0;
        for (int j = 0; j < 2; ++j) {
            char c = hex[2*i+j];
            int digit = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
            if (digit < 0) return false;
            value = value * 16 + digit;
        }
        if (!value) return false;
        out[i] = (char)value;
    }
    out[n/2] = 0;
    return true;
}
static cJSON *reply(const char *id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", true);
    return root;
}
static void fail(cJSON *root, const char *error)
{
    cJSON_ReplaceItemInObject(root, "ok", cJSON_CreateBool(false));
    cJSON_AddStringToObject(root, "error", error);
}
static void worker(void *unused)
{
    (void)unused;
    job_t job;
    while (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) {
        char *save = NULL;
        char *id = strtok_r(job.text, " ", &save);
        char *op = strtok_r(NULL, " ", &save);
        cJSON *root = reply(id ? id : "");
        bool restart = false;
        if (!op) fail(root, "invalid_request");
        else if (!strcmp(op, "status")) {
            char ssid[33], pass[65], hex[65];
            cfg_wifi_get(ssid, sizeof(ssid), pass, sizeof(pass));
            memset(pass, 0, sizeof(pass)); hex_encode(ssid, hex, 32);
            cJSON_AddStringToObject(root, "ssid_hex", hex);
            cJSON_AddBoolToObject(root, "connected", net_dev_sta_connected());
            cJSON_AddStringToObject(root, "ip", net_dev_ip());
            cJSON_AddStringToObject(root, "ap_ip", net_dev_ap_ip());
            cJSON_AddBoolToObject(root, "armed", ble_ota_window_for(job.usb ? OTA_ALLOW_USB : OTA_ALLOW_BLE));
        } else if (!strcmp(op, "scan") || !strcmp(op, "page")) {
            unsigned page = 0;
            esp_err_t err = ESP_OK;
            if (!strcmp(op, "scan")) {
                if (ble_ota_in_progress()) err = ESP_ERR_INVALID_STATE;
                else err = esp_wifi_scan_start(NULL, true);
                s_count = 20;
                if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&s_count, s_networks);
                if (err != ESP_OK) s_count = 0;
            } else {
                char *arg = strtok_r(NULL, " ", &save);
                page = arg ? strtoul(arg, NULL, 10) : 0;
            }
            if (err != ESP_OK) fail(root, "scan_failed");
            else if (page > 6) fail(root, "invalid_page");
            else {
                cJSON *list = cJSON_AddArrayToObject(root, "networks");
                for (unsigned i = page * 3; i < s_count && i < page * 3 + 3; ++i) {
                    char name[33] = {0}, hex[65]; memcpy(name, s_networks[i].ssid, 32); hex_encode(name, hex, 32);
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "ssid_hex", hex);
                    cJSON_AddNumberToObject(item, "rssi", s_networks[i].rssi);
                    cJSON_AddNumberToObject(item, "auth", s_networks[i].authmode);
                    cJSON_AddItemToArray(list, item);
                }
                cJSON_AddNumberToObject(root, "page", page);
                cJSON_AddNumberToObject(root, "pages", (s_count + 2) / 3);
            }
        } else if (!strcmp(op, "save") || !strcmp(op, "clear")) {
            if (!job.usb && !ble_ota_window_for(OTA_ALLOW_BLE)) fail(root, "window_closed");
            else if (ble_ota_in_progress()) fail(root, "device_busy");
            else {
                esp_err_t err = ESP_ERR_INVALID_ARG;
                if (!strcmp(op, "clear")) err = cfg_wifi_clear();
                else {
                    char *ssid_hex = strtok_r(NULL, " ", &save), *pass_hex = strtok_r(NULL, " ", &save);
                    char ssid[33] = {0}, pass[65] = {0};
                    if (ssid_hex && pass_hex && hex_decode(ssid_hex, ssid, 32) && ssid[0] &&
                        (!strcmp(pass_hex, "-") || hex_decode(pass_hex, pass, 63)) &&
                        (!pass[0] || strlen(pass) >= 8)) err = cfg_wifi_set(ssid, pass);
                    memset(pass, 0, sizeof(pass));
                }
                if (err != ESP_OK) fail(root, "save_failed");
                else { cJSON_AddBoolToObject(root, "restarting", true); restart = true; }
            }
        } else fail(root, "unsupported_operation");
        char encoded[REPLY_SIZE];
        if (!cJSON_PrintPreallocated(root, encoded, sizeof(encoded), false))
            snprintf(encoded, sizeof(encoded), "{\"id\":\"%s\",\"ok\":false,\"error\":\"reply_too_large\"}", id ? id : "");
        cJSON_Delete(root); memset(&job, 0, sizeof(job));
        portENTER_CRITICAL(&s_lock);
        memcpy(s_reply, encoded, strlen(encoded) + 1); s_busy = false;
        portEXIT_CRITICAL(&s_lock);
        if (restart) cfg_reboot_later(1500);
    }
}
esp_err_t wifi_setup_init(void)
{
    if (s_jobs) return ESP_OK;
    s_jobs = xQueueCreate(1, sizeof(job_t));
    if (!s_jobs) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "wifi_setup", 6144, NULL, 4, NULL) == pdPASS) return ESP_OK;
    vQueueDelete(s_jobs);
    s_jobs = NULL;
    return ESP_ERR_NO_MEM;
}
esp_err_t wifi_setup_submit(const char *request, bool usb)
{
    if (!request) return ESP_ERR_INVALID_ARG;
    size_t length = strnlen(request, REQUEST_SIZE);
    if (!s_jobs || length < 10 || length >= REQUEST_SIZE || request[8] != ' ') return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < 8; ++i) if (!((request[i] >= '0' && request[i] <= '9') || (request[i] >= 'a' && request[i] <= 'f'))) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_busy) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; }
    s_busy = true;
    snprintf(s_reply, sizeof(s_reply), "{\"id\":\"%.8s\",\"busy\":true}", request);
    portEXIT_CRITICAL(&s_lock);
    job_t job = {.usb = usb}; memcpy(job.text, request, length + 1);
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        s_busy = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
size_t wifi_setup_response(char *out, size_t capacity)
{
    if (!out || !capacity) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t length = strlcpy(out, s_reply, capacity);
    portEXIT_CRITICAL(&s_lock);
    return length < capacity ? length : capacity - 1;
}
#define WIFI_UUID(n) BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,n,0x00,0x40,0x6e)
static const ble_uuid128_t svc_uuid = WIFI_UUID(0x30), request_uuid = WIFI_UUID(0x31), reply_uuid = WIFI_UUID(0x32);
static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char buffer[REPLY_SIZE]; size_t length = wifi_setup_response(buffer, sizeof(buffer));
        return os_mbuf_append(ctx->om, buffer, length) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (s_rx_conn != conn) { s_rx_len = 0; s_rx_conn = conn; }
    size_t length = OS_MBUF_PKTLEN(ctx->om);
    if (!length || s_rx_len + length >= sizeof(s_rx)) { s_rx_len = 0; return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; }
    os_mbuf_copydata(ctx->om, 0, length, s_rx + s_rx_len); s_rx_len += length; s_rx[s_rx_len] = 0;
    if (s_rx[s_rx_len - 1] == '\n') {
        s_rx[--s_rx_len] = 0;
        esp_err_t err = wifi_setup_submit(s_rx, false);
        memset(s_rx, 0, sizeof(s_rx)); s_rx_len = 0;
        if (err != ESP_OK) return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}
static const struct ble_gatt_chr_def chars[] = {
    {.uuid=&request_uuid.u, .access_cb=access_cb, .flags=BLE_GATT_CHR_F_WRITE},
    {.uuid=&reply_uuid.u, .access_cb=access_cb, .flags=BLE_GATT_CHR_F_READ}, {0}
};
static const struct ble_gatt_svc_def services[] = {
    {.type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=&svc_uuid.u, .characteristics=chars}, {0}
};
const struct ble_gatt_svc_def *wifi_setup_services(void) { return services; }
void wifi_setup_disconnected(unsigned conn) { if (s_rx_conn == conn) { memset(s_rx, 0, sizeof(s_rx)); s_rx_len = 0; s_rx_conn = BLE_HS_CONN_HANDLE_NONE; } }
