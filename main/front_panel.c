#include "front_panel.h"
#include "sdkconfig.h"

#ifdef CONFIG_FRONT_PANEL_ENABLED
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "app_cfg.h"
#include "front_panel_lcd.h"
#include "front_panel_pins.h"
#include "web_config.h"

enum { UP, DOWN, LEFT, RIGHT, OK, BACK, KEY_COUNT };
enum { ENTRY_LIMIT = 16, VISIBLE_ENTRIES = 8, DEBOUNCE_SAMPLES = 3 };
static const char *TAG = "front_panel";
static const char *MOUNT_POINT = "/sd";
static const int key_pins[KEY_COUNT] = {
    KEY_UP_GPIO, KEY_DOWN_GPIO, KEY_LEFT_GPIO, KEY_RIGHT_GPIO, KEY_OK_GPIO, KEY_BACK_GPIO,
};
static const char *const key_names[KEY_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "OK", "BACK"};

typedef struct {
    bool candidate;
    bool pressed;
    unsigned samples;
    unsigned presses;
} panel_key_t;

/* All mount, directory and key state belongs to the single front panel task. */
static sdmmc_card_t *s_card;
static const char *s_card_status = "SD: READY TO MOUNT";
static char s_entries[ENTRY_LIMIT][PANEL_COLS - 1];
static unsigned s_count, s_selected;
static bool s_more_entries, s_directory_error, s_key_page, s_lcd_ready;
static panel_key_t s_keys[KEY_COUNT];
static char s_web_code[WEB_CONFIG_CODE_LEN + 1];

static void draw(void)
{
    if (!s_lcd_ready) return;
    panel_lines_t lines = {0};
    snprintf(lines[0], sizeof(lines[0]), "ESP-SWD %.10s", esp_app_get_description()->version);
    if (s_web_code[0]) snprintf(lines[1], sizeof(lines[1]), "WEB: %s", s_web_code);
    if (s_key_page) {
        strcpy(lines[2], "KEY PRESS COUNTS");
        for (unsigned i = 0; i < KEY_COUNT; ++i) {
            snprintf(lines[i + 4], sizeof(lines[0]), "%-5s %u", key_names[i], s_keys[i].presses);
        }
        strcpy(lines[18], "LEFT/RIGHT: SD");
    } else {
        snprintf(lines[2], sizeof(lines[0]), "%s", s_card_status);
        if (s_card) {
            const uint64_t mib = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024);
            snprintf(lines[3], sizeof(lines[0]), "%llu MIB", (unsigned long long)mib);
            if (s_directory_error) {
                strcpy(lines[5], "ROOT READ ERROR");
            } else if (s_count == 0) {
                strcpy(lines[5], "ROOT IS EMPTY");
            } else {
                unsigned first = s_selected / VISIBLE_ENTRIES * VISIBLE_ENTRIES;
                for (unsigned i = first; i < s_count && i < first + VISIBLE_ENTRIES; ++i) {
                    snprintf(lines[5 + i - first], sizeof(lines[0]), "%c %s",
                             i == s_selected ? '>' : ' ', s_entries[i]);
                }
                if (s_more_entries) strcpy(lines[14], "FIRST 16 ENTRIES");
            }
            strcpy(lines[16], "UP/DOWN: SELECT");
            strcpy(lines[17], "OK: REFRESH");
            strcpy(lines[18], "BACK: EJECT");
        } else {
            strcpy(lines[17], "OK: MOUNT");
        }
        strcpy(lines[19], "LEFT/RIGHT: KEYS");
    }
    esp_err_t err = front_panel_lcd_show(lines);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD draw failed: %s", esp_err_to_name(err));
        s_lcd_ready = false;
    }
}

static void read_directory(void)
{
    s_count = s_selected = 0;
    s_more_entries = s_directory_error = false;
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        s_directory_error = true;
        ESP_LOGW(TAG, "Cannot open SD root: errno=%d", errno);
        return;
    }
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            s_directory_error = errno != 0;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (s_count == ENTRY_LIMIT) {
            s_more_entries = true;
            break;
        }
        const char *prefix = entry->d_type == DT_DIR ? "/" : "";
        snprintf(s_entries[s_count], sizeof(s_entries[0]), "%s%.*s", prefix,
                 (int)(sizeof(s_entries[0]) - 1 - strlen(prefix)), entry->d_name);
        ++s_count;
    }
    closedir(dir);
}

static void mount_card(void)
{
    s_card_status = "SD: MOUNTING";
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
    const esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 4,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &config, &card);
    if (err != ESP_OK) {
        s_card_status = "SD: MOUNT FAILED";
        ESP_LOGW(TAG, "SDMMC mount failed: %s", esp_err_to_name(err));
        return;
    }
    s_card = card;
    s_card_status = "SD: MOUNTED";
    ESP_LOGI(TAG, "SDMMC 1-bit mounted at %s", MOUNT_POINT);
    read_directory();
}

static void eject_card(void)
{
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;
    s_count = s_selected = 0;
    s_card_status = err == ESP_OK ? "SD: EJECTED" : "SD: EJECT ERROR";
    if (err != ESP_OK) ESP_LOGW(TAG, "SDMMC unmount failed: %s", esp_err_to_name(err));
}

static void handle_press(unsigned key)
{
    ++s_keys[key].presses;
    ESP_LOGD(TAG, "Key %s", key_names[key]);
    if (key == LEFT || key == RIGHT) {
        s_key_page = !s_key_page;
    } else if (!s_key_page) {
        if (key == UP && s_selected > 0) --s_selected;
        if (key == DOWN && s_selected + 1 < s_count) ++s_selected;
        if (key == OK) {
            if (s_card) read_directory();
            else mount_card();
        }
        if (key == BACK && s_card) eject_card();
    }
}

static void front_panel_task(void *arg)
{
    (void)arg;
    esp_err_t err = front_panel_lcd_init();
    s_lcd_ready = err == ESP_OK;
    if (err != ESP_OK) ESP_LOGW(TAG, "LCD init failed: %s", esp_err_to_name(err));
    mount_card();
    draw();
    for (;;) {
        char web_code[WEB_CONFIG_CODE_LEN + 1];
        web_config_get_code(web_code);
        bool dirty = strcmp(s_web_code, web_code) != 0;
        if (dirty) strcpy(s_web_code, web_code);
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
                if (pressed) {
                    handle_press(i);
                    dirty = true;
                }
            }
        }
        if (dirty) draw();
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
