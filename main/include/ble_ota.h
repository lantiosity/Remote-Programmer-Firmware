#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

struct ble_gatt_svc_def;
esp_err_t ble_ota_init(void);
/* Call only after nimble_port_init(); USB/WiFi core does not require NimBLE. */
void ble_ota_enable_notifications(void);
const struct ble_gatt_svc_def *ble_ota_services(void);
void ble_ota_disconnected(uint16_t conn);
bool ble_ota_in_progress(void);
esp_err_t ble_ota_arm(uint32_t seconds);
void ble_ota_status(uint8_t out[20]);
size_t ble_ota_info(char *out, size_t capacity);
void ble_ota_boot_confirm(bool healthy);

/* Shared OTA engine. Owners below 0xFFFD are BLE connection handles.
 * USB/TCP adapters use the reserved owners and the same window/session lock. */
#define OTA_OWNER_USB 0xFFFDu
#define OTA_OWNER_WIFI 0xFFFEu
#define OTA_MAX_PAYLOAD 1028u
esp_err_t ble_ota_submit(uint16_t owner, uint8_t op, const uint8_t *payload, size_t length);
