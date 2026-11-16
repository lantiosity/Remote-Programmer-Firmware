#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
struct ble_gatt_svc_def;
esp_err_t wifi_setup_init(void);
esp_err_t wifi_setup_submit(const char *request, bool usb_console);
size_t wifi_setup_response(char *out, size_t capacity);
const struct ble_gatt_svc_def *wifi_setup_services(void);
void wifi_setup_disconnected(unsigned conn);
