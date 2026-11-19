#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_ota.h"
#include "target_flash_h743.h"
#include "web_jobs.h"

typedef struct {
    web_job_kind_t kind;
    uint8_t *image;
    size_t size;
    uint32_t address;
    bool reset_run;
} web_job_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static web_job_status_t s_status = {.phase = "idle"};

web_job_status_t web_job_status(void)
{
    portENTER_CRITICAL(&s_lock);
    web_job_status_t status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return status;
}

static void progress(const char *phase, size_t done, size_t total)
{
    portENTER_CRITICAL(&s_lock);
    s_status.phase = phase;
    s_status.done = done;
    s_status.total = total;
    portEXIT_CRITICAL(&s_lock);
}

static void worker(void *arg)
{
    web_job_t job = *(web_job_t *)arg;
    free(arg);
    esp_err_t err;
    if (job.kind == WEB_JOB_FLASH) {
        err = target_flash_h743(job.address, job.image, job.size, job.reset_run, progress);
        ble_ota_release_reboot();
    } else {
        err = ble_ota_web_update(job.image, job.size, progress);
    }
    free(job.image);

    portENTER_CRITICAL(&s_lock);
    s_status.error = err;
    if (err == ESP_OK) {
        s_status.done = job.size;
        s_status.phase = job.kind == WEB_JOB_OTA ? "rebooting" : "done";
    }
    s_status.active = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

esp_err_t web_job_start(web_job_kind_t kind, uint32_t address, bool reset_run,
                        uint8_t *image, size_t size, uint32_t *id)
{
    web_job_t *job = malloc(sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    *job = (web_job_t){.kind = kind, .image = image, .size = size,
                      .address = address, .reset_run = reset_run};

    portENTER_CRITICAL(&s_lock);
    if (s_status.active) {
        portEXIT_CRITICAL(&s_lock);
        free(job);
        return ESP_ERR_INVALID_STATE;
    }
    web_job_status_t previous = s_status;
    *id = previous.id + 1;
    s_status = (web_job_status_t){.id = *id, .kind = kind, .phase = "queued",
                                   .total = size, .active = true};
    portEXIT_CRITICAL(&s_lock);

    /* Target SWD owns CPU1 GPIO and must finish before a configuration reboot. */
    esp_err_t err = kind == WEB_JOB_FLASH ? ble_ota_reserve_reboot() : ESP_OK;
    if (err == ESP_OK) {
        if (xTaskCreatePinnedToCore(worker, "web_job", 6144, job, 4, NULL, 1) == pdPASS)
            return ESP_OK;
        if (kind == WEB_JOB_FLASH) ble_ota_release_reboot();
        err = ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    s_status = previous;
    portEXIT_CRITICAL(&s_lock);
    free(job);
    return err;
}
