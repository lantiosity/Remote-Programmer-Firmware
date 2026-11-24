#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* CMSIS-DAP vendor command 0x80; target UART CDC is untouched. */
uint32_t wired_ota_usb(const uint8_t *req, size_t length, uint8_t *resp, size_t capacity);
esp_err_t wired_ota_net_start(void);
