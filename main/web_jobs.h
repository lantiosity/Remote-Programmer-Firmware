#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { WEB_JOB_NONE, WEB_JOB_FLASH, WEB_JOB_OTA } web_job_kind_t;

typedef struct {
    uint32_t id;
    web_job_kind_t kind;
    const char *phase;
    size_t done;
    size_t total;
    esp_err_t error;
    bool active;
} web_job_status_t;

web_job_status_t web_job_status(void);
/* Takes ownership of image on success. */
esp_err_t web_job_start(web_job_kind_t kind, uint32_t address, bool reset_run,
                        uint8_t *image, size_t size, uint32_t *id);
