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
/* Reserve a non-OTA reboot before persisting configuration or starting its timer. */
esp_err_t ble_ota_reserve_reboot(void);
void ble_ota_release_reboot(void);
esp_err_t ble_ota_arm(uint32_t seconds);
#define OTA_ALLOW_USB 1u
#define OTA_ALLOW_WIFI 2u
#define OTA_ALLOW_BLE 4u
#define OTA_ALLOW_ALL 7u
esp_err_t ble_ota_arm_transport(uint32_t seconds, unsigned transports);
bool ble_ota_window_for(unsigned transport);
void ble_ota_status(uint8_t out[20]);
/* Same protocol v1 payload, with armed/state scoped to this adapter's owner. */
void ble_ota_status_for(uint16_t owner, uint8_t out[20]);
size_t ble_ota_info(char *out, size_t capacity);
void ble_ota_boot_confirm(bool healthy);

/* Reserved transport owners for the shared update engine. */
#define OTA_OWNER_USB 0xFFFDu
#define OTA_OWNER_WIFI 0xFFFEu
#define OTA_MAX_PAYLOAD 1028u
esp_err_t ble_ota_submit(uint16_t owner, uint8_t op, const uint8_t *payload, size_t length);

/* The caller has staged a complete image and authenticated its upload. */
esp_err_t ble_ota_web_update(const uint8_t *image, size_t size,
                            void (*report)(const char *, size_t, size_t));
