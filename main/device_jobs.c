#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_ota.h"
#include "target_flash_h743.h"
#include "device_jobs.h"

typedef struct {
    device_job_kind_t kind;
    uint8_t *image;
    size_t size;
    uint32_t address;
    bool reset_run;
    offline_image_t source;
} device_job_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static device_job_status_t s_status = {.phase = "idle"};
static device_job_status_t s_panel_status = {.phase = "idle"};
static bool s_panel_job;

device_job_status_t device_job_status(void)
{
    portENTER_CRITICAL(&s_lock);
    device_job_status_t status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return status;
}

device_job_status_t device_job_panel_status(void)
{
    portENTER_CRITICAL(&s_lock);
    device_job_status_t status = s_panel_status;
    portEXIT_CRITICAL(&s_lock);
    return status;
}

static void progress(const char *phase, size_t done, size_t total)
{
    portENTER_CRITICAL(&s_lock);
    s_status.phase = phase;
    s_status.done = done;
    s_status.total = total;
    if (s_panel_job) s_panel_status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

static void worker(void *arg)
{
    device_job_t job = *(device_job_t *)arg;
    free(arg);
    esp_err_t err = job.source.size ? offline_image_load(&job.source, &job.image, progress) : ESP_OK;
    if (err == ESP_OK) {
        switch (job.kind) {
        case DEVICE_JOB_FLASH:
            err = target_flash_h743(job.address, job.image, job.size, job.reset_run, progress);
            break;
        case DEVICE_JOB_SAVE:
            err = offline_image_save(&job.source, job.image, progress);
            break;
        default:
            err = ble_ota_web_update(job.image, job.size, progress);
            break;
        }
    }
    free(job.image);
    if (job.kind != DEVICE_JOB_OTA) ble_ota_release_reboot();

    portENTER_CRITICAL(&s_lock);
    s_status.error = err;
    if (err == ESP_OK) {
        s_status.done = job.size;
        s_status.total = job.size;
        s_status.phase = job.kind == DEVICE_JOB_OTA ? "rebooting" : "done";
    }
    s_status.active = false;
    if (s_panel_job) s_panel_status = s_status;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static esp_err_t start_job(device_job_t *job, uint32_t *id)
{
    portENTER_CRITICAL(&s_lock);
    if (s_status.active) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    device_job_status_t previous = s_status;
    device_job_status_t previous_panel = s_panel_status;
    *id = previous.id + 1;
    s_panel_job = job->source.size != 0;
    s_status = (device_job_status_t){.id = *id, .kind = job->kind, .phase = "queued",
                                   .total = job->size, .active = true};
    if (s_panel_job) s_panel_status = s_status;
    portEXIT_CRITICAL(&s_lock);

    /* SWD owns CPU1 GPIO. Neither SD nor target writes may span an OTA reboot. */
    const bool reserve = job->kind != DEVICE_JOB_OTA;
    esp_err_t err = reserve ? ble_ota_reserve_reboot() : ESP_OK;
    if (err == ESP_OK) {
        if (xTaskCreatePinnedToCore(worker, "device_job", 6144, job, 4, NULL, 1) == pdPASS)
            return ESP_OK;
        if (reserve) ble_ota_release_reboot();
        err = ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    s_status = previous;
    s_panel_status = previous_panel;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t device_job_start(device_job_kind_t kind, uint32_t address, bool reset_run,
                           uint8_t *image, size_t size, uint32_t *id)
{
    device_job_t *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    *job = (device_job_t){.kind = kind, .image = image, .size = size,
                          .address = address, .reset_run = reset_run};
    esp_err_t err = start_job(job, id);
    if (err != ESP_OK) free(job);
    return err;
}

esp_err_t device_job_start_offline(const offline_image_t *source, bool save,
                                   uint32_t address, bool reset_run)
{
    if (!source || !target_flash_h743_range(save ? H743_FLASH_BASE : address, source->size) ||
        (save && source->saved)) return ESP_ERR_INVALID_ARG;
    device_job_t *job = malloc(sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    *job = (device_job_t){.kind = save ? DEVICE_JOB_SAVE : DEVICE_JOB_FLASH,
                          .size = source->size, .address = address,
                          .reset_run = reset_run, .source = *source};
    uint32_t id;
    esp_err_t err = start_job(job, &id);
    if (err != ESP_OK) free(job);
    return err;
}
