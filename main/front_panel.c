#include "front_panel.h"
#include "sdkconfig.h"

#ifdef CONFIG_FRONT_PANEL_ENABLED
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "app_cfg.h"
#include "device_jobs.h"
#include "front_panel_lcd.h"
#include "front_panel_pins.h"
#include "front_panel_view.h"
#include "net_dev.h"
#include "offline_image.h"
#include "web_config.h"

enum { UP, DOWN, LEFT, RIGHT, OK, BACK, KEY_COUNT };
enum { FILE_LIMIT = 48, DEBOUNCE_SAMPLES = 3, CONFIRM_US = 1100000 };
static const char *TAG = "front_panel";
static const int key_pins[KEY_COUNT] = {
    KEY_UP_GPIO, KEY_DOWN_GPIO, KEY_LEFT_GPIO, KEY_RIGHT_GPIO, KEY_OK_GPIO, KEY_BACK_GPIO,
};

typedef struct {
    bool candidate, pressed;
    unsigned samples, presses;
} panel_key_t;

static sdmmc_card_t *s_card;
static panel_card_t s_card_status;
static panel_page_t s_page, s_result_next;
static panel_key_t s_keys[KEY_COUNT];
static char (*s_files)[OFFLINE_NAME_SIZE];
static unsigned s_file_count, s_file_selection, s_home_selection, s_field;
static bool s_more, s_directory_error, s_lcd_ready, s_reset_run, s_saving;
static offline_image_t s_image;
static uint32_t s_address;
static int64_t s_hold_started;
static const char *s_result_detail;
static char s_result_stage[64], s_web_code[WEB_CONFIG_CODE_LEN + 1];
static bool s_success;
static panel_scene_t s_scene;

static const char *phase_label(const char *phase)
{
    if (!strcmp(phase, "load")) return "读取镜像";
    if (!strcmp(phase, "connect")) return "连接目标";
    if (!strcmp(phase, "erase")) return "擦除目标扇区";
    if (!strcmp(phase, "program")) return "写入目标";
    if (!strcmp(phase, "verify")) return "逐字节读回校验";
    if (!strcmp(phase, "cache_erase")) return "准备板载存储";
    if (!strcmp(phase, "cache_write")) return "保存并读回校验";
    return "准备任务";
}

static unsigned job_percent(const device_job_status_t *job)
{
    unsigned phase = job->total ? 100 * job->done / job->total : 0;
    if (!strcmp(job->phase, "load")) return (s_saving ? 20 : 15) * phase / 100;
    if (!strcmp(job->phase, "cache_erase")) return 20 + 30 * phase / 100;
    if (!strcmp(job->phase, "cache_write")) return 50 + 49 * phase / 100;
    if (!strcmp(job->phase, "verify")) return 85 + 14 * phase / 100;
    if (!strcmp(job->phase, "program") || !strcmp(job->phase, "erase"))
        return 18 + 67 * phase / 100;
    if (!strcmp(job->phase, "connect")) return 15;
    return 0;
}

static void draw(void)
{
    if (!s_lcd_ready) return;
    panel_view_t view = {
        .page = s_page, .card = s_card_status, .version = esp_app_get_description()->version,
        .ip = net_dev_sta_connected() ? net_dev_ip() : "", .web_code = s_web_code,
        .filename = s_image.name, .size = s_image.size, .saved = s_image.saved,
        .saving = s_saving, .reset_run = s_reset_run, .address = s_address,
        .success = s_success, .stage = s_result_stage, .detail = s_result_detail,
        .more = s_more, .directory_error = s_directory_error,
    };
    switch (s_page) {
    case PANEL_HOME:
        view.selected = s_home_selection;
        break;
    case PANEL_FILES: {
        const unsigned first = s_file_selection / PANEL_FILES_VISIBLE * PANEL_FILES_VISIBLE;
        view.page_index = first / PANEL_FILES_VISIBLE;
        view.pages = (s_file_count + PANEL_FILES_VISIBLE - 1) / PANEL_FILES_VISIBLE;
        view.selected = s_file_selection - first;
        for (unsigned i = 0; i < PANEL_FILES_VISIBLE && first + i < s_file_count; ++i)
            view.files[i] = s_files[first + i];
        break;
    }
    case PANEL_SETUP:
        view.selected = s_field;
        break;
    case PANEL_CONFIRM:
        view.last_address = s_address +
            (s_image.size + H743_SECTOR_SIZE - 1) / H743_SECTOR_SIZE * H743_SECTOR_SIZE - 1;
        if (s_hold_started) {
            int64_t elapsed = esp_timer_get_time() - s_hold_started;
            view.percent = elapsed >= CONFIRM_US ? 100 : (unsigned)(elapsed * 100 / CONFIRM_US);
        }
        break;
    case PANEL_WORKING: {
        device_job_status_t job = device_job_panel_status();
        view.stage = phase_label(job.phase);
        view.percent = job_percent(&job);
        break;
    }
    case PANEL_STATUS:
        for (unsigned i = 0; i < KEY_COUNT; ++i) view.key_counts[i] = s_keys[i].presses;
        break;
    case PANEL_RESULT:
        break;
    }
    front_panel_compose(&s_scene, &view);
    esp_err_t err = front_panel_lcd_show(&s_scene);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD draw failed: %s", esp_err_to_name(err));
        s_lcd_ready = false;
        s_hold_started = 0;
    }
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(a, b);
}

static void read_directory(void)
{
    s_file_count = s_file_selection = 0;
    s_more = s_directory_error = false;
    if (!s_files) { s_directory_error = true; return; }
    DIR *dir = opendir(OFFLINE_SD_ROOT);
    if (!dir) { s_directory_error = true; return; }
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(dir);
        if (!entry) {
            s_directory_error = errno != 0;
            break;
        }
        if (entry->d_type == DT_DIR || !offline_sd_candidate(entry->d_name)) continue;
        if (s_file_count == FILE_LIMIT) {
            s_more = true;
            break;
        }
        strcpy(s_files[s_file_count++], entry->d_name);
    }
    closedir(dir);
    qsort(s_files, s_file_count, sizeof(*s_files), compare_names);
}

static void mount_card(void)
{
    s_card_status = PANEL_CARD_LOADING;
    draw();
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags &= ~(SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_8BIT | SDMMC_HOST_FLAG_DDR);
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
    slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
    const esp_vfs_fat_sdmmc_mount_config_t config = {.format_if_mount_failed = false, .max_files = 4};
    esp_err_t err = esp_vfs_fat_sdmmc_mount(OFFLINE_SD_ROOT, &host, &slot, &config, &s_card);
    if (err != ESP_OK) {
        s_card_status = PANEL_CARD_ERROR;
        ESP_LOGW(TAG, "SDMMC mount failed: %s", esp_err_to_name(err));
        return;
    }
    s_card_status = PANEL_CARD_READY;
    ESP_LOGI(TAG, "SDMMC 1-bit mounted at %s", OFFLINE_SD_ROOT);
}

static void eject_card(void)
{
    esp_err_t err = esp_vfs_fat_sdcard_unmount(OFFLINE_SD_ROOT, s_card);
    s_card = NULL;
    s_file_count = s_file_selection = 0;
    s_card_status = err == ESP_OK ? PANEL_CARD_EMPTY : PANEL_CARD_ERROR;
    if (err != ESP_OK) ESP_LOGW(TAG, "SDMMC unmount failed: %s", esp_err_to_name(err));
}

static void browse(void)
{
    s_page = PANEL_FILES;
    if (s_card && s_directory_error) eject_card();
    if (!s_card) mount_card();
    if (s_card) read_directory();
}

static const char *error_detail(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE: return "设备忙或文件已变化";
    case ESP_ERR_NOT_SUPPORTED: return "型号或存储布局不匹配";
    case ESP_ERR_INVALID_CRC: return "读回数据不一致";
    case ESP_ERR_NOT_ALLOWED: return "受保护的目标或存储";
    case ESP_ERR_NOT_FOUND: return s_image.saved ? "没有板载镜像" : "找不到存储卡镜像";
    case ESP_ERR_INVALID_SIZE: return "镜像大小无效";
    case ESP_ERR_NO_MEM: return "可用内存不足";
    case ESP_ERR_TIMEOUT: return "操作超时";
    default: return "请检查接线或存储卡";
    }
}

static void show_result(esp_err_t err, const char *stage, panel_page_t next)
{
    s_page = PANEL_RESULT;
    s_result_next = next;
    s_success = err == ESP_OK;
    if (s_success) {
        s_result_detail = s_saving ? "可从主页打开板载镜像" :
            s_reset_run ? "已复位启动目标" : "目标保持暂停";
        snprintf(s_result_stage, sizeof(s_result_stage), "%s", s_saving ? "镜像读回一致" : "镜像逐字节一致");
    } else {
        s_result_detail = error_detail(err);
        snprintf(s_result_stage, sizeof(s_result_stage), "%s  %04X", stage, (unsigned)err);
        ESP_LOGW(TAG, "Panel operation failed: %s", esp_err_to_name(err));
    }
}

static void select_image(bool saved)
{
    s_image = (offline_image_t){.saved = saved};
    s_saving = false;
    if (!saved) strcpy(s_image.name, s_files[s_file_selection]);
    esp_err_t err = saved ? offline_saved_inspect(&s_image) : offline_sd_inspect(s_image.name, &s_image);
    if (err != ESP_OK) {
        show_result(err, "选择镜像", saved ? PANEL_HOME : PANEL_FILES);
        return;
    }
    s_address = H743_FLASH_BASE;
    s_reset_run = false;
    s_field = 2;
    s_page = PANEL_SETUP;
}

static void adjust_setup(unsigned key)
{
    if (s_field == 0) {
        unsigned count = (H743_FLASH_SIZE - s_image.size) / H743_SECTOR_SIZE + 1;
        unsigned current = (s_address - H743_FLASH_BASE) / H743_SECTOR_SIZE;
        current = (current + (key == LEFT ? count - 1 : 1)) % count;
        s_address = H743_FLASH_BASE + current * H743_SECTOR_SIZE;
    } else if (s_field == 1) {
        s_reset_run = !s_reset_run;
    }
}

static void handle_press(unsigned key)
{
    ++s_keys[key].presses;
    switch (s_page) {
    case PANEL_HOME:
        if (key == UP && s_home_selection) --s_home_selection;
        if (key == DOWN && s_home_selection < 2) ++s_home_selection;
        if (key == OK) {
            if (s_home_selection == 0) browse();
            else if (s_home_selection == 1) select_image(true);
            else s_page = PANEL_STATUS;
        }
        break;
    case PANEL_FILES:
        if (key == BACK) s_page = PANEL_HOME;
        if (!s_file_count) {
            if (key == OK) browse();
            break;
        }
        if (key == UP && s_file_selection) --s_file_selection;
        if (key == DOWN && s_file_selection + 1 < s_file_count) ++s_file_selection;
        if (key == LEFT && s_file_selection >= PANEL_FILES_VISIBLE) s_file_selection -= PANEL_FILES_VISIBLE;
        if (key == RIGHT && s_file_selection / PANEL_FILES_VISIBLE < (s_file_count - 1) / PANEL_FILES_VISIBLE) {
            s_file_selection += PANEL_FILES_VISIBLE;
            if (s_file_selection >= s_file_count) s_file_selection = s_file_count - 1;
        }
        if (key == OK) select_image(false);
        break;
    case PANEL_SETUP:
        if (key == BACK) s_page = s_image.saved ? PANEL_HOME : PANEL_FILES;
        if (key == UP && s_field) --s_field;
        if (key == DOWN && s_field < (s_image.saved ? 2u : 3u)) ++s_field;
        if (key == LEFT || key == RIGHT || (key == OK && s_field < 2)) adjust_setup(key);
        if (key == OK && s_field >= 2) {
            s_saving = s_field == 3;
            s_page = PANEL_CONFIRM;
            s_hold_started = 0;
        }
        break;
    case PANEL_CONFIRM:
        if (key == OK) s_hold_started = esp_timer_get_time();
        if (key == BACK) {
            s_hold_started = 0;
            s_page = PANEL_SETUP;
        }
        break;
    case PANEL_WORKING:
        break;
    case PANEL_RESULT:
        if (key == BACK) s_page = PANEL_HOME;
        if (key == OK) s_page = s_result_next;
        break;
    case PANEL_STATUS:
        if (key == BACK) s_page = PANEL_HOME;
        if (key == RIGHT && s_card) eject_card();
        if (key == OK && !s_card) mount_card();
        break;
    }
}

static bool read_keys(void)
{
    bool dirty = false;
    for (unsigned i = 0; i < KEY_COUNT; ++i) {
        panel_key_t *key = &s_keys[i];
        const bool pressed = gpio_get_level(key_pins[i]) == 0;
        if (pressed != key->candidate) {
            key->candidate = pressed;
            key->samples = 1;
        } else if (key->samples < DEBOUNCE_SAMPLES) {
            ++key->samples;
        }
        if (key->samples == DEBOUNCE_SAMPLES && key->pressed != pressed) {
            key->pressed = pressed;
            if (pressed) handle_press(i);
            else if (i == OK) s_hold_started = 0;
            dirty = true;
        }
    }
    return dirty;
}

static void front_panel_task(void *arg)
{
    (void)arg;
    esp_err_t err = front_panel_lcd_init();
    s_lcd_ready = err == ESP_OK;
    if (!s_lcd_ready) {
        ESP_LOGW(TAG, "LCD init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    s_files = heap_caps_malloc(FILE_LIMIT * sizeof(*s_files), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    draw();
    mount_card();
    draw();
    int64_t next_frame = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        char code[WEB_CONFIG_CODE_LEN + 1];
        web_config_get_code(code);
        bool dirty = strcmp(s_web_code, code) != 0;
        if (dirty) strcpy(s_web_code, code);
        if (s_lcd_ready) dirty |= read_keys();
        if (s_page == PANEL_CONFIRM && s_hold_started && s_keys[OK].candidate &&
            now - s_hold_started >= CONFIRM_US) {
            s_hold_started = 0;
            err = device_job_start_offline(&s_image, s_saving, s_address, s_reset_run);
            if (err == ESP_OK) s_page = PANEL_WORKING;
            else show_result(err, "准备任务", PANEL_SETUP);
            dirty = true;
        }
        if (s_page == PANEL_WORKING) {
            device_job_status_t job = device_job_panel_status();
            if (!job.active) {
                show_result(job.error, phase_label(job.phase), PANEL_SETUP);
                dirty = true;
            }
        }
        if (now >= next_frame && (s_page == PANEL_WORKING || s_page == PANEL_STATUS || s_hold_started))
            dirty = true;
        if (dirty) {
            draw();
            next_frame = now + 100000;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static bool pins_available(void)
{
    const cfg_probe_pins_t *probe = cfg_probe();
    const int core_pins[] = {
        probe->swclk_gpio, probe->swdio_gpio, probe->nreset_gpio,
        cfg_uart_tx_gpio(), cfg_uart_rx_gpio(), CONFIG_BLE_OTA_BUTTON_GPIO,
    };
    const int panel_pins[] = {
        LCD_SCK, LCD_MOSI, LCD_CS, LCD_DC, LCD_RST, LCD_BL, SD_CLK, SD_CMD, SD_D0,
        KEY_UP_GPIO, KEY_DOWN_GPIO, KEY_LEFT_GPIO, KEY_RIGHT_GPIO, KEY_OK_GPIO, KEY_BACK_GPIO,
    };
    for (unsigned i = 0; i < sizeof(panel_pins) / sizeof(panel_pins[0]); ++i) {
        for (unsigned j = 0; j < sizeof(core_pins) / sizeof(core_pins[0]); ++j) {
            if (panel_pins[i] == core_pins[j]) {
                ESP_LOGW(TAG, "Front panel disabled: GPIO%d already assigned", panel_pins[i]);
                return false;
            }
        }
    }
    return true;
}
#endif

void front_panel_start(void)
{
#ifdef CONFIG_FRONT_PANEL_ENABLED
    if (!pins_available()) return;
    gpio_config_t keys = {.mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    for (unsigned i = 0; i < KEY_COUNT; ++i) keys.pin_bit_mask |= 1ULL << key_pins[i];
    esp_err_t err = gpio_config(&keys);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Button GPIO init failed: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(front_panel_task, "front_panel", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Cannot start front panel task");
    }
#endif
}
