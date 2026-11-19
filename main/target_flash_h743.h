#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define H743_FLASH_BASE UINT32_C(0x08000000)
#define H743_FLASH_SIZE UINT32_C(0x00200000)
#define H743_SECTOR_SIZE UINT32_C(0x00020000)

typedef void (*flash_progress_t)(const char *phase, size_t done, size_t total);

bool target_flash_h743_range(uint32_t address, size_t size);
/* Runs on CPU1. Owns probe_lock for the duration of the operation. */
esp_err_t target_flash_h743(uint32_t address, const uint8_t *image, size_t size,
                            bool reset_run, flash_progress_t report);
