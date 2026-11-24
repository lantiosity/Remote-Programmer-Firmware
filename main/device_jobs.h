#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "offline_image.h"

typedef enum { DEVICE_JOB_NONE, DEVICE_JOB_FLASH, DEVICE_JOB_OTA, DEVICE_JOB_SAVE } device_job_kind_t;

typedef struct {
    uint32_t id;
    device_job_kind_t kind;
    const char *phase;
    size_t done;
    size_t total;
    esp_err_t error;
    bool active;
} device_job_status_t;

device_job_status_t device_job_status(void);
device_job_status_t device_job_panel_status(void);
/* Takes ownership of image on success. */
esp_err_t device_job_start(device_job_kind_t kind, uint32_t address, bool reset_run,
                        uint8_t *image, size_t size, uint32_t *id);
esp_err_t device_job_start_offline(const offline_image_t *source, bool save,
                                   uint32_t address, bool reset_run);
