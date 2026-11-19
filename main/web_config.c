#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/platform_util.h"

#include "app_cfg.h"
#include "net_dev.h"
#include "web_config.h"

static const char *TAG = "web_config";
static const char HEX[] = "0123456789ABCDEF";
static const int64_t WINDOW_US = 300LL * 1000000;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;
static int64_t s_expires;
static char s_code[WEB_CONFIG_CODE_LEN + 1];

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

esp_err_t web_config_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
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
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            httpd_stop(server);
            return err;
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_ready = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Web configuration: http://%s/", net_dev_ap_ip());
    return ESP_OK;
}
