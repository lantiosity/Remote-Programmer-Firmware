#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "target_flash_h743.h"

#define OFFLINE_SD_ROOT "/sd"
#define OFFLINE_NAME_SIZE 256

typedef struct {
    char name[OFFLINE_NAME_SIZE];
    size_t size;
    uint32_t generation;
    bool saved;
} offline_image_t;

bool offline_sd_candidate(const char *name);
esp_err_t offline_sd_inspect(const char *name, offline_image_t *image);
esp_err_t offline_saved_inspect(offline_image_t *image);

/* The caller owns the PSRAM image returned on success. */
esp_err_t offline_image_load(const offline_image_t *source, uint8_t **out, flash_progress_t report);
esp_err_t offline_image_save(const offline_image_t *source, const uint8_t *image, flash_progress_t report);
