#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/platform_util.h"

#include "app_cfg.h"
#include "ble_ota.h"
#include "net_dev.h"
#include "serial_bridge.h"
#include "target_flash_h743.h"
#include "web_config.h"
#include "device_jobs.h"
#include "wifi_setup.h"

static const char *TAG = "web_config";
static const char HEX[] = "0123456789ABCDEF";
static const int64_t WINDOW_US = 300LL * 1000000;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;
static int64_t s_expires;
static char s_code[WEB_CONFIG_CODE_LEN + 1];

#define SERIAL_CAPACITY 8192u
static portMUX_TYPE s_serial_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_serial;
static uint32_t s_serial_end, s_serial_used;

extern const char page_start[] asm("_binary_web_config_html_start");
extern const char page_end[] asm("_binary_web_config_html_end");

static void to_hex(const uint8_t *bytes, size_t length, char *out)
{
    for (size_t i = 0; i < length; ++i) {
        out[2 * i] = HEX[bytes[i] >> 4];
        out[2 * i + 1] = HEX[bytes[i] & 15];
    }
    out[2 * length] = '\0';
}

bool web_config_arm(void)
{
    uint8_t random[WEB_CONFIG_CODE_LEN / 2];
    char code[WEB_CONFIG_CODE_LEN + 1];
    esp_fill_random(random, sizeof(random));
    to_hex(random, sizeof(random), code);
    int64_t expires = esp_timer_get_time() + WINDOW_US;
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready;
    if (ready) {
        memcpy(s_code, code, sizeof(s_code));
        s_expires = expires;
    }
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

void web_config_close(void)
{
    portENTER_CRITICAL(&s_lock);
    s_expires = 0;
    memset(s_code, 0, sizeof(s_code));
    portEXIT_CRITICAL(&s_lock);
}

bool web_config_get_code(char code[WEB_CONFIG_CODE_LEN + 1])
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool open = s_ready && now < s_expires;
    if (open) memcpy(code, s_code, sizeof(s_code));
    else code[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    return open;
}

/* Support numeric AP/STA addresses and reject foreign Host/Origin headers. */
static bool local_request(httpd_req_t *req)
{
    char host[24], origin[40], expected[40];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    if (httpd_req_get_hdr_value_len(req, "Origin")) {
        if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
        snprintf(expected, sizeof(expected), "http://%s", host);
        if (strcmp(origin, expected)) return false;
    }
    char *port = strchr(host, ':');
    if (port) {
        if (strcmp(port, ":80")) return false;
        *port = '\0';
    }
    return strcmp(host, net_dev_ap_ip()) == 0 ||
        (net_dev_sta_connected() && strcmp(host, net_dev_ip()) == 0);
}

static bool authorized(httpd_req_t *req)
{
    char code[WEB_CONFIG_CODE_LEN + 1], received[WEB_CONFIG_CODE_LEN + 1];
    if (!web_config_get_code(code) ||
        httpd_req_get_hdr_value_len(req, "X-Setup-Code") != WEB_CONFIG_CODE_LEN ||
        httpd_req_get_hdr_value_str(req, "X-Setup-Code", received, sizeof(received)) != ESP_OK) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < WEB_CONFIG_CODE_LEN; ++i) diff |= (uint8_t)code[i] ^ (uint8_t)received[i];
    return diff == 0;
}

static void response_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

static esp_err_t page(httpd_req_t *req)
{
    response_headers(req);
    if (!local_request(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, NULL);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
        "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    return httpd_resp_send(req, page_start, page_end - page_start);
}

static esp_err_t status(httpd_req_t *req)
{
    response_headers(req);
    if (!local_request(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, NULL);
    char ssid[33], ssid_hex[65], code[WEB_CONFIG_CODE_LEN + 1], body[320];
    cfg_wifi_get(ssid, sizeof(ssid), NULL, 0);
    to_hex((const uint8_t *)ssid, strlen(ssid), ssid_hex);
    snprintf(body, sizeof(body),
        "{\"version\":\"%s\",\"serial\":\"%s\",\"ip\":\"%s\",\"ap_ip\":\"%s\","
        "\"connected\":%s,\"ssid_hex\":\"%s\",\"setup_open\":%s}",
        esp_app_get_description()->version, cfg_serial_number(), net_dev_ip(), net_dev_ap_ip(),
        net_dev_sta_connected() ? "true" : "false", ssid_hex,
        web_config_get_code(code) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    return -1;
}

static bool from_hex(const cJSON *field, char *out, size_t capacity)
{
    if (!cJSON_IsString(field)) return false;
    const char *hex = field->valuestring;
    size_t length = strlen(hex);
    if ((length & 1) || length / 2 >= capacity) return false;
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_digit(hex[i]), low = hex_digit(hex[i + 1]);
        if (high < 0 || low < 0 || !(high | low)) return false;
        out[i / 2] = (char)((high << 4) | low);
    }
    out[length / 2] = '\0';
    return true;
}

static esp_err_t save_request(httpd_req_t *req)
{
    char body[256];
    if (!req->content_len || req->content_len >= sizeof(body)) return ESP_ERR_INVALID_SIZE;
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 4000000;
    while (received < req->content_len) {
        if (esp_timer_get_time() >= deadline) break;
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) break;
        received += (size_t)count;
    }
    if (received != req->content_len) {
        mbedtls_platform_zeroize(body, sizeof(body));
        return ESP_ERR_TIMEOUT;
    }
    body[received] = '\0';
    if (memchr(body, 0, received)) {
        mbedtls_platform_zeroize(body, sizeof(body));
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *json = cJSON_ParseWithLengthOpts(body, received + 1, NULL, true);
    mbedtls_platform_zeroize(body, sizeof(body));
    const cJSON *network = cJSON_GetObjectItemCaseSensitive(json, "ssid_hex");
    cJSON *key = cJSON_GetObjectItemCaseSensitive(json, "password_hex");
    char ssid[33], pass[64];
    bool valid = from_hex(network, ssid, sizeof(ssid)) && from_hex(key, pass, sizeof(pass));
    if (cJSON_IsString(key)) mbedtls_platform_zeroize(key->valuestring, strlen(key->valuestring));
    cJSON_Delete(json);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (valid) {
        err = authorized(req) ? cfg_wifi_set(ssid, pass, 2500) : ESP_ERR_NOT_ALLOWED;
    }
    mbedtls_platform_zeroize(pass, sizeof(pass));
    return err;
}

static esp_err_t change_wifi(httpd_req_t *req)
{
    response_headers(req);
    if (!local_request(req) || !authorized(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Setup code missing, invalid or expired");
        return ESP_FAIL;
    }
    esp_err_t err;
    if (req->user_ctx) {
        err = req->content_len == 0 ? cfg_wifi_clear(2500) : ESP_ERR_INVALID_ARG;
    } else {
        char type[32];
        err = httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type));
        err = err == ESP_OK && strcmp(type, "application/json") == 0 ?
            save_request(req) : ESP_ERR_INVALID_ARG;
    }
    switch (err) {
    case ESP_OK:
        web_config_close();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"restarting\":true}");
    case ESP_ERR_NOT_ALLOWED:
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Setup code expired");
        break;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi settings");
        break;
    case ESP_ERR_TIMEOUT:
        httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request timed out");
        break;
    case ESP_ERR_INVALID_STATE:
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "Device is upgrading or restarting");
        break;
    default:
        ESP_LOGW(TAG, "Wi-Fi settings operation failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not apply settings");
        break;
    }
    return ESP_FAIL;
}

static bool read_u32_header(httpd_req_t *req, const char *name, uint32_t *value)
{
    char raw[11];
    if (httpd_req_get_hdr_value_str(req, name, raw, sizeof(raw)) != ESP_OK || !raw[0])
        return false;
    uint64_t number = 0;
    for (const char *p = raw; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        number = number * 10 + (unsigned)(*p - '0');
    }
    if (number > UINT32_MAX) return false;
    *value = (uint32_t)number;
    return true;
}

static esp_err_t upload_error(httpd_req_t *req, httpd_err_code_t code, const char *reason)
{
    httpd_resp_send_err(req, code, reason);
    return ESP_FAIL; /* Close the socket if a rejected request still has an unread body. */
}

static esp_err_t upload_conflict(httpd_req_t *req)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, "Device is busy");
    return ESP_FAIL;
}

static esp_err_t wifi_scan(httpd_req_t *req)
{
    response_headers(req);
    if (!local_request(req) || !authorized(req))
        return upload_error(req, HTTPD_403_FORBIDDEN, "Setup code missing, invalid or expired");

    httpd_resp_set_type(req, "application/json");
    if (req->method == HTTP_POST) {
        if (req->content_len)
            return upload_error(req, HTTPD_400_BAD_REQUEST, "Expected an empty request");
        if (device_job_status().active || ble_ota_in_progress()) return upload_conflict(req);
        uint32_t id;
        esp_err_t err = wifi_setup_web_scan_start(&id);
        if (err == ESP_ERR_INVALID_STATE) return upload_conflict(req);
        if (err != ESP_OK)
            return upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start Wi-Fi scan");
        char body[32];
        snprintf(body, sizeof(body), "{\"id\":%" PRIu32 "}", id);
        httpd_resp_set_status(req, "202 Accepted");
        return httpd_resp_sendstr(req, body);
    }

    wifi_setup_scan_t scan;
    wifi_setup_web_scan_status(&scan);
    if (scan.error == ESP_ERR_INVALID_STATE) return upload_conflict(req);
    if (scan.error != ESP_OK)
        return upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi scan failed");

    const size_t capacity = 128 + 128 * scan.count;
    char *body = malloc(capacity);
    if (!body) return upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan buffer unavailable");
    size_t length = snprintf(body, capacity, "{\"id\":%" PRIu32 ",\"running\":%s,\"networks\":[",
        scan.id, scan.running ? "true" : "false");
    for (uint16_t i = 0; i < scan.count; ++i) {
        const wifi_setup_network_t *network = &scan.networks[i];
        char ssid_hex[65];
        to_hex(network->ssid, network->length, ssid_hex);
        length += snprintf(body + length, capacity - length,
            "%s{\"ssid_hex\":\"%s\",\"rssi\":%d,\"channel\":%u,\"open\":%s}",
            i ? "," : "", ssid_hex, (int)network->rssi, (unsigned)network->channel,
            network->open ? "true" : "false");
    }
    memcpy(body + length, "]}", 2);
    esp_err_t err = httpd_resp_send(req, body, length + 2);
    free(body);
    return err;
}

static bool ota_header_valid(const uint8_t *image)
{
    esp_image_header_t header;
    esp_app_desc_t desc;
    memcpy(&header, image, sizeof(header));
    memcpy(&desc, image + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(desc));
    return header.magic == ESP_IMAGE_HEADER_MAGIC && header.chip_id == ESP_CHIP_ID_ESP32S3 &&
        desc.magic_word == ESP_APP_DESC_MAGIC_WORD &&
        strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) == 0;
}

static esp_err_t upload(httpd_req_t *req)
{
    response_headers(req);
    if (!local_request(req) || !authorized(req))
        return upload_error(req, HTTPD_403_FORBIDDEN, "Setup code missing, invalid or expired");
    if (device_job_status().active || ble_ota_in_progress()) return upload_conflict(req);
    char type[32];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/octet-stream"))
        return upload_error(req, HTTPD_400_BAD_REQUEST, "Expected a binary image");

    const bool ota = req->user_ctx != NULL;
    const size_t size = req->content_len;
    uint32_t address = 0, reset_run = 0;
    if (ota) {
        const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
        if (!slot || size < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
            sizeof(esp_app_desc_t) || size > slot->size)
            return upload_error(req, HTTPD_400_BAD_REQUEST, "Image does not fit the OTA slot");
    } else {
        if (!read_u32_header(req, "X-Flash-Address", &address) ||
            !read_u32_header(req, "X-Reset-Run", &reset_run) || reset_run > 1 ||
            !target_flash_h743_range(address, size))
            return upload_error(req, HTTPD_400_BAD_REQUEST, "Invalid target address or image size");
        if (cfg_probe()->nreset_gpio < 0)
            return upload_error(req, HTTPD_400_BAD_REQUEST, "Target NRST is required");
    }
    uint8_t *image = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image) return upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Image buffer unavailable");

    const int64_t deadline = esp_timer_get_time() + 45000000;
    size_t received = 0;
    while (received < size && esp_timer_get_time() < deadline) {
        size_t count = size - received < 4096 ? size - received : 4096;
        int n = httpd_req_recv(req, (char *)image + received, count);
        if (n <= 0) break;
        received += (size_t)n;
    }
    const bool allowed = received == size && authorized(req);
    if (!allowed || (ota && !ota_header_valid(image))) {
        free(image);
        if (received != size)
            return upload_error(req, HTTPD_408_REQ_TIMEOUT, "Upload incomplete or timed out");
        if (!allowed)
            return upload_error(req, HTTPD_403_FORBIDDEN, "Setup code expired");
        return upload_error(req, HTTPD_400_BAD_REQUEST, "Expected an ESP-SWD OTA app image");
    }

    uint32_t id;
    esp_err_t err = device_job_start(ota ? DEVICE_JOB_OTA : DEVICE_JOB_FLASH, address, reset_run, image, size, &id);
    if (err != ESP_OK) {
        free(image);
        if (err == ESP_ERR_INVALID_STATE) return upload_conflict(req);
        return upload_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start the job");
    }
    char body[32];
    snprintf(body, sizeof(body), "{\"id\":%" PRIu32 "}", id);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t job_status(httpd_req_t *req)
{
    response_headers(req);
    if (!local_request(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, NULL);
    device_job_status_t job = device_job_status();
    const char *kind = job.kind == DEVICE_JOB_FLASH ? "flash" : job.kind == DEVICE_JOB_OTA ? "ota" : "";
    char body[224];
    snprintf(body, sizeof(body),
        "{\"id\":%" PRIu32 ",\"kind\":\"%s\",\"phase\":\"%s\",\"done\":%zu,\"total\":%zu,"
        "\"error\":%d,\"error_name\":\"%s\",\"active\":%s}", job.id, kind, job.phase, job.done, job.total,
        (int)job.error, esp_err_to_name(job.error), job.active ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t ota_info(httpd_req_t *req)
{
    response_headers(req);
    if (!local_request(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, NULL);
    char body[512];
    size_t size = ble_ota_info(body, sizeof(body));
    if (!size) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, size);
}

static bool serial_sink(int opaque, const uint8_t *data, size_t size)
{
    (void)opaque;
    portENTER_CRITICAL(&s_serial_lock);
    size_t index = s_serial_end % SERIAL_CAPACITY;
    size_t first = size < SERIAL_CAPACITY - index ? size : SERIAL_CAPACITY - index;
    memcpy(s_serial + index, data, first);
    memcpy(s_serial, data + first, size - first);
    s_serial_end += size;
    s_serial_used += size;
    if (s_serial_used > SERIAL_CAPACITY) s_serial_used = SERIAL_CAPACITY;
    portEXIT_CRITICAL(&s_serial_lock);
    return true;
}

static esp_err_t serial_read(httpd_req_t *req)
{
    response_headers(req);
    if (!local_request(req) || !authorized(req))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Setup code missing, invalid or expired");
    if (!s_serial) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "UART unavailable");
    uint32_t cursor;
    bool first_read = !httpd_req_get_hdr_value_len(req, "X-Serial-Cursor");
    if (!first_read && !read_u32_header(req, "X-Serial-Cursor", &cursor))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid serial cursor");

    char data[2048], next[11], missed[11], baud[12];
    portENTER_CRITICAL(&s_serial_lock);
    if (first_read) cursor = s_serial_end - s_serial_used;
    uint32_t count = s_serial_end - cursor;
    /* A cursor ahead of the writer is stale (for example after a reboot). */
    if (count > INT32_MAX) count = s_serial_used;
    uint32_t dropped = count > s_serial_used ? count - s_serial_used : 0;
    cursor = s_serial_end - (count - dropped);
    count -= dropped;
    if (count > sizeof(data)) count = sizeof(data);
    size_t index = cursor % SERIAL_CAPACITY;
    size_t first = count < SERIAL_CAPACITY - index ? count : SERIAL_CAPACITY - index;
    memcpy(data, s_serial + index, first);
    memcpy(data + first, s_serial, count - first);
    portEXIT_CRITICAL(&s_serial_lock);

    snprintf(next, sizeof(next), "%" PRIu32, cursor + count);
    snprintf(missed, sizeof(missed), "%" PRIu32, dropped);
    snprintf(baud, sizeof(baud), "%d", serial_bridge_get_baud());
    httpd_resp_set_hdr(req, "X-Serial-Next", next);
    httpd_resp_set_hdr(req, "X-Serial-Dropped", missed);
    httpd_resp_set_hdr(req, "X-Serial-Baud", baud);
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, data, count);
}

esp_err_t web_config_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.max_uri_handlers = 11;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    config.stack_size = 6144;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = change_wifi},
        {.uri = "/api/wifi/clear", .method = HTTP_POST, .handler = change_wifi, .user_ctx = (void *)1},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = wifi_scan},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan},
        {.uri = "/api/flash", .method = HTTP_POST, .handler = upload},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = upload, .user_ctx = (void *)1},
        {.uri = "/api/job", .method = HTTP_GET, .handler = job_status},
        {.uri = "/api/ota-info", .method = HTTP_GET, .handler = ota_info},
        {.uri = "/api/serial", .method = HTTP_GET, .handler = serial_read},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            httpd_stop(server);
            return err;
        }
    }
    s_serial = heap_caps_malloc(SERIAL_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_serial || serial_bridge_subscribe(serial_sink, 0) != ESP_OK) {
        free(s_serial);
        s_serial = NULL;
        ESP_LOGW(TAG, "Web serial monitor unavailable");
    }
    portENTER_CRITICAL(&s_lock);
    s_ready = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Web configuration: http://%s/", net_dev_ap_ip());
    return ESP_OK;
}
