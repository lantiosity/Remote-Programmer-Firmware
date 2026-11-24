#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ESP32-S3 image header, followed by the first segment header and app descriptor. */
static inline bool esp_swdlink_image(const uint8_t *image, size_t size)
{
    static const uint8_t chip_id[] = {9, 0};
    static const uint8_t app_magic[] = {0x32, 0x54, 0xcd, 0xab};
    static const char project[] = "esp_swdlink";
    return image && size >= 112 && image[0] == 0xe9 &&
        memcmp(image + 12, chip_id, sizeof(chip_id)) == 0 &&
        memcmp(image + 32, app_magic, sizeof(app_magic)) == 0 &&
        memcmp(image + 80, project, sizeof(project)) == 0;
}
