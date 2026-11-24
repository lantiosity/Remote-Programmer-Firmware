#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_SETUP_SCAN_MAX 20

typedef struct {
    uint8_t ssid[32];
    uint8_t length, channel;
    int8_t rssi;
    bool open;
} wifi_setup_network_t;

typedef struct {
    uint32_t id;
    bool running;
    esp_err_t error;
    uint16_t count;
    wifi_setup_network_t networks[WIFI_SETUP_SCAN_MAX];
} wifi_setup_scan_t;

struct ble_gatt_svc_def;
esp_err_t wifi_setup_init(void);
esp_err_t wifi_setup_submit(const char *request, bool usb_console);
size_t wifi_setup_response(char *out, size_t capacity);
esp_err_t wifi_setup_web_scan_start(uint32_t *id);
void wifi_setup_web_scan_status(wifi_setup_scan_t *result);
const struct ble_gatt_svc_def *wifi_setup_services(void);
void wifi_setup_disconnected(unsigned conn);
