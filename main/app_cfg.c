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
#include "nvs.h"
#include "nvs_flash.h"

#include "app_cfg.h"

static const char *TAG = "cfg";

#define NVS_NAMESPACE "swdlink"
#define KEY_SSID      "ssid"
#define KEY_PASS      "pass"
#define KEY_BAUD      "baud"

static const cfg_probe_pins_t s_pins = {
    .swclk_gpio  = CONFIG_PROBE_SWCLK_GPIO,
    .swdio_gpio  = CONFIG_PROBE_SWDIO_GPIO,
    .nreset_gpio = CONFIG_PROBE_NRESET_GPIO,
    .clock_hz    = (uint32_t)CONFIG_PROBE_DEFAULT_CLOCK_KHZ * 1000u,
};

static char s_serial[24];
static char s_ssid[33];
static char s_pass[65];
static int  s_baud = CONFIG_TARGET_UART_BAUD;

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
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "ESP-SWD-%02X%02X%02X", mac[3], mac[4], mac[5]);

    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    if (strlen(CONFIG_LINK_WIFI_SSID) > 0) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", CONFIG_LINK_WIFI_SSID);
        snprintf(s_pass, sizeof(s_pass), "%s", CONFIG_LINK_WIFI_PASSWORD);
    }

    nvs_handle_t h = open_nvs();
    if (h == 0) {
        return ESP_OK;
    }
    char tmp_ssid[33] = {0};
    char tmp_pass[65] = {0};
    load_str(h, KEY_SSID, tmp_ssid, sizeof(tmp_ssid));
    load_str(h, KEY_PASS, tmp_pass, sizeof(tmp_pass));
    if (strlen(tmp_ssid) > 0) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", tmp_ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", tmp_pass);
    }
    int32_t baud = 0;
    if (nvs_get_i32(h, KEY_BAUD, &baud) == ESP_OK && baud > 0) {
        s_baud = (int)baud;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "序列号=%s, SWD=%d kHz, WiFi=%s, UART baud=%d",
             s_serial, (int)(s_pins.clock_hz / 1000),
             s_ssid[0] ? s_ssid : "(未配置, 仅热点)", s_baud);
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
    if (ssid && ssid_len) {
        snprintf(ssid, ssid_len, "%s", s_ssid);
    }
    if (pass && pass_len) {
        snprintf(pass, pass_len, "%s", s_pass);
    }
    return s_ssid[0] != '\0';
}

esp_err_t cfg_wifi_set(const char *ssid, const char *pass)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
    nvs_handle_t h = open_nvs();
    if (h == 0) {
        return ESP_FAIL;
    }
    nvs_set_str(h, KEY_SSID, s_ssid);
    nvs_set_str(h, KEY_PASS, s_pass);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t cfg_wifi_clear(void)
{
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    nvs_handle_t h = open_nvs();
    if (h == 0) {
        return ESP_FAIL;
    }
    nvs_erase_key(h, KEY_SSID);
    nvs_erase_key(h, KEY_PASS);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
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

void cfg_reboot_later(uint32_t delay_ms)
{
    esp_timer_handle_t timer = NULL;
    const esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .name = "reboot",
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)delay_ms * 1000u);
    }
}
