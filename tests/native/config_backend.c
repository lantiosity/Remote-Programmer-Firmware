#include <stdbool.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"

static uint8_t stored[128], staging[128];
static size_t stored_length, staged_length;
bool mock_ota_active, mock_reboot_reserved, mock_timer_scheduled;
bool mock_reserved_during_write, mock_reserved_during_timer;
bool mock_erase_without_reservation;
unsigned mock_writes, mock_commits;
unsigned mock_legacy_keys, mock_erase_calls, mock_erase_error_on, mock_commit_error_on;
int mock_write_error, mock_commit_error, mock_timer_error;

void test_cfg_reset(void)
{
    stored_length = staged_length = 0;
    mock_ota_active = mock_reboot_reserved = mock_timer_scheduled = false;
    mock_reserved_during_write = mock_reserved_during_timer = false;
    mock_erase_without_reservation = false;
    mock_writes = mock_commits = 0;
    mock_legacy_keys = 3;
    mock_erase_calls = mock_erase_error_on = mock_commit_error_on = 0;
    mock_write_error = mock_commit_error = mock_timer_error = 0;
}

esp_err_t esp_read_mac(uint8_t out[6], int type) { memset(out, 0, 6); return ESP_OK; }
void esp_restart(void) { }
const char *esp_err_to_name(esp_err_t err) { return "mock error"; }

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    *out = &mock_timer_scheduled;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay_us)
{
    mock_reserved_during_timer = mock_reboot_reserved;
    if (mock_timer_error) return mock_timer_error;
    mock_timer_scheduled = true;
    return ESP_OK;
}

esp_err_t ble_ota_reserve_reboot(void)
{
    if (mock_ota_active || mock_reboot_reserved) return ESP_ERR_INVALID_STATE;
    mock_reboot_reserved = true;
    return ESP_OK;
}

void ble_ota_release_reboot(void) { mock_reboot_reserved = false; }

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out) { *out = 1; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { staged_length = 0; }

static unsigned legacy_key(const char *key)
{
    if (!strcmp(key, "ssid")) return 1;
    if (!strcmp(key, "pass")) return 2;
    return 0;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *length)
{
    unsigned mask = legacy_key(key);
    if (!(mock_legacy_keys & mask)) return ESP_ERR_NVS_NOT_FOUND;
    const char *value = mask == 1 ? "legacy" : "legacy-pass";
    if (strlen(value) >= *length) return ESP_ERR_INVALID_ARG;
    strcpy(out, value);
    return ESP_OK;
}

esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *out) { return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_i32(nvs_handle_t handle, const char *key, int32_t value) { return ESP_OK; }

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    if (!stored_length) return ESP_ERR_NVS_NOT_FOUND;
    if (strcmp(key, "wifi_v1") || *length < stored_length) return ESP_ERR_INVALID_ARG;
    memcpy(out, stored, stored_length);
    *length = stored_length;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    ++mock_writes;
    mock_reserved_during_write = mock_reboot_reserved;
    if (mock_write_error) return mock_write_error;
    if (strcmp(key, "wifi_v1") || length > sizeof(staging)) return ESP_ERR_INVALID_ARG;
    memcpy(staging, value, length);
    staged_length = length;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    ++mock_commits;
    if (mock_commit_error && (!mock_commit_error_on || mock_commits == mock_commit_error_on)) return mock_commit_error;
    if (staged_length) {
        memcpy(stored, staging, staged_length);
        stored_length = staged_length;
    }
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    ++mock_erase_calls;
    if (!mock_reboot_reserved) mock_erase_without_reservation = true;
    unsigned mask = legacy_key(key);
    if (!(mock_legacy_keys & mask)) return ESP_ERR_NVS_NOT_FOUND;
    if (mock_erase_error_on == mask) return ESP_FAIL;
    mock_legacy_keys &= ~mask;
    return ESP_OK;
}
