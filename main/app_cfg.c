/**
 * @file  app_cfg.c
 * @brief  配置管理：引脚、串口参数、WiFi 凭据（NVS）、设备序列号。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#include "app_cfg.h"
#include "ble_ota.h"

static const char *TAG = "cfg";

#define NVS_NAMESPACE "swdlink"
#define KEY_SSID      "ssid"
#define KEY_PASS      "pass"
#define KEY_WIFI      "wifi_v1"
#define KEY_BAUD      "baud"

static const cfg_probe_pins_t s_pins = {
    .swclk_gpio  = CONFIG_PROBE_SWCLK_GPIO,
    .swdio_gpio  = CONFIG_PROBE_SWDIO_GPIO,
    .nreset_gpio = CONFIG_PROBE_NRESET_GPIO,
    .clock_hz    = (uint32_t)CONFIG_PROBE_DEFAULT_CLOCK_KHZ * 1000u,
};

static char s_serial[24];
typedef struct {
    char ssid[33];
    char pass[65];
} wifi_credentials_t;

static wifi_credentials_t s_wifi;
static portMUX_TYPE s_wifi_lock = portMUX_INITIALIZER_UNLOCKED;
static int  s_baud = CONFIG_TARGET_UART_BAUD;
static esp_timer_handle_t s_reboot_timer;
static void reboot_timer_cb(void *arg);

static nvs_handle_t open_nvs(void)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        return 0;
    }
    return h;
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_len)
{
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK) {
        dst[0] = '\0';
    }
}

esp_err_t cfg_init(void)
{
    if (!s_reboot_timer) {
        const esp_timer_create_args_t timer_args = {.callback=reboot_timer_cb, .name="reboot"};
        esp_err_t err = esp_timer_create(&timer_args, &s_reboot_timer);
        if (err != ESP_OK) return err;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "ESP-SWD-%02X%02X%02X", mac[3], mac[4], mac[5]);

    s_wifi = (wifi_credentials_t){0};
    snprintf(s_wifi.ssid, sizeof(s_wifi.ssid), "%s", CONFIG_LINK_WIFI_SSID);
    snprintf(s_wifi.pass, sizeof(s_wifi.pass), "%s", CONFIG_LINK_WIFI_PASSWORD);

    nvs_handle_t h = open_nvs();
    if (h == 0) {
        return ESP_OK;
    }
    wifi_credentials_t saved = {0};
    size_t length = sizeof(saved);
    esp_err_t err = nvs_get_blob(h, KEY_WIFI, &saved, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        load_str(h, KEY_SSID, saved.ssid, sizeof(saved.ssid));
        load_str(h, KEY_PASS, saved.pass, sizeof(saved.pass));
        if (saved.ssid[0]) s_wifi = saved;
    } else if (err == ESP_OK && length == sizeof(saved) &&
               saved.ssid[sizeof(saved.ssid) - 1] == '\0' && saved.pass[sizeof(saved.pass) - 1] == '\0') {
        s_wifi = saved;
    } else {
        ESP_LOGW(TAG, "读取 WiFi 凭据失败: %s", err == ESP_OK ? "invalid record" : esp_err_to_name(err));
    }
    int32_t baud = 0;
    if (nvs_get_i32(h, KEY_BAUD, &baud) == ESP_OK && baud > 0) {
        s_baud = (int)baud;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "序列号=%s, SWD=%d kHz, WiFi=%s, UART baud=%d",
             s_serial, (int)(s_pins.clock_hz / 1000),
             s_wifi.ssid[0] ? s_wifi.ssid : "(未配置, 仅热点)", s_baud);
    return ESP_OK;
}

const cfg_probe_pins_t *cfg_probe(void)
{
    return &s_pins;
}

int cfg_uart_port(void)
{
    return CONFIG_TARGET_UART_PORT;
}

int cfg_uart_tx_gpio(void)
{
    return CONFIG_TARGET_UART_TX_GPIO;
}

int cfg_uart_rx_gpio(void)
{
    return CONFIG_TARGET_UART_RX_GPIO;
}

int cfg_uart_baud(void)
{
    return s_baud;
}

esp_err_t cfg_uart_set_baud(int baud)
{
    if (baud <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_baud = baud;
    nvs_handle_t h = open_nvs();
    if (h) {
        nvs_set_i32(h, KEY_BAUD, baud);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

bool cfg_wifi_get(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    portENTER_CRITICAL(&s_wifi_lock);
    wifi_credentials_t wifi = s_wifi;
    portEXIT_CRITICAL(&s_wifi_lock);
    if (ssid && ssid_len) {
        snprintf(ssid, ssid_len, "%s", wifi.ssid);
    }
    if (pass && pass_len) {
        snprintf(pass, pass_len, "%s", wifi.pass);
    }
    return wifi.ssid[0] != '\0';
}

static esp_err_t schedule_reserved_reboot(uint32_t delay_ms)
{
    return esp_timer_start_once(s_reboot_timer, (uint64_t)delay_ms * 1000u);
}

static esp_err_t erase_legacy_wifi_key(nvs_handle_t h, const char *key)
{
    esp_err_t err = nvs_erase_key(h, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t save_wifi(const wifi_credentials_t *wifi, uint32_t reboot_delay_ms)
{
    if (!s_reboot_timer || !reboot_delay_ms) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ble_ota_reserve_reboot();
    if (err != ESP_OK) return err;
    nvs_handle_t h = open_nvs();
    err = ESP_FAIL;
    if (h) {
        err = nvs_set_blob(h, KEY_WIFI, wifi, sizeof(*wifi));
        if (err == ESP_OK) err = nvs_commit(h);
        /* Publish the empty record before touching legacy keys so this
         * firmware never falls back to a partially cleared legacy pair. */
        if (err == ESP_OK && !wifi->ssid[0]) {
            err = erase_legacy_wifi_key(h, KEY_SSID);
            if (err == ESP_OK) err = erase_legacy_wifi_key(h, KEY_PASS);
            if (err == ESP_OK) err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_wifi_lock);
        s_wifi = *wifi;
        portEXIT_CRITICAL(&s_wifi_lock);
        err = schedule_reserved_reboot(reboot_delay_ms);
    }
    if (err != ESP_OK) ble_ota_release_reboot();
    return err;
}

esp_err_t cfg_wifi_set(const char *ssid, const char *pass, uint32_t reboot_delay_ms)
{
    wifi_credentials_t wifi = {0};
    size_t pass_len = pass ? strlen(pass) : 0;
    if (!ssid || !ssid[0] || strlen(ssid) >= sizeof(wifi.ssid) ||
        (pass_len && pass_len < 8) || pass_len >= sizeof(wifi.pass)) return ESP_ERR_INVALID_ARG;
    snprintf(wifi.ssid, sizeof(wifi.ssid), "%s", ssid);
    snprintf(wifi.pass, sizeof(wifi.pass), "%s", pass ? pass : "");
    return save_wifi(&wifi, reboot_delay_ms);
}

esp_err_t cfg_wifi_clear(uint32_t reboot_delay_ms)
{
    /* An empty record overrides compile-time defaults on this firmware. */
    const wifi_credentials_t empty = {0};
    return save_wifi(&empty, reboot_delay_ms);
}

const char *cfg_serial_number(void)
{
    return s_serial;
}

const char *cfg_ble_name(void)
{
    return CONFIG_LINK_BLE_NAME;
}

uint16_t cfg_dap_tcp_port(void)
{
    return (uint16_t)CONFIG_LINK_TCP_PORT;
}

uint16_t cfg_serial_tcp_port(void)
{
    return (uint16_t)CONFIG_LINK_SERIAL_TCP_PORT;
}

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

esp_err_t cfg_reboot_later(uint32_t delay_ms)
{
    if (!s_reboot_timer || !delay_ms) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ble_ota_reserve_reboot();
    if (err != ESP_OK) return err;
    err = schedule_reserved_reboot(delay_ms);
    if (err != ESP_OK) ble_ota_release_reboot();
    return err;
}
