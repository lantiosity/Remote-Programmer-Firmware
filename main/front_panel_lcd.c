#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "front_panel_lcd.h"
#include "front_panel_pins.h"

enum { LCD_WIDTH = 240, ROW_HEIGHT = 16, GLYPH_ADVANCE = 12 };
enum { COLOR_BG = 0x10a3, COLOR_TEXT = 0xffff, COLOR_TITLE = 0x07ff };

_Static_assert(PANEL_ROWS * ROW_HEIGHT == 320, "LCD rows must cover the full display");
_Static_assert(PANEL_COLS * GLYPH_ADVANCE + 6 <= LCD_WIDTH, "LCD text must fit the DMA strip");

/* 5x7 glyphs, one bit column per byte, least significant bit at the top. */
static const uint8_t letters[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x7f,0x20,0x18,0x20,0x7f}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43},
};
static const uint8_t digits[10][5] = {
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
};

static uint8_t glyph(char ch, unsigned col)
{
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'][col];
    if (ch >= '0' && ch <= '9') return digits[ch - '0'][col];
    switch (ch) {
    case ' ': return 0;
    case '.': return col == 2 ? 0x40 : 0;
    case ':': return col == 2 ? 0x24 : 0;
    case '-': return 0x08;
    case '_': return 0x40;
    case '/': return 0x40 >> col;
    case '>': return ((const uint8_t[]){0,0x22,0x14,0x08,0})[col];
    default: return ((const uint8_t[]){0x02,0x01,0x51,0x09,0x06})[col];
    }
}

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_dma_done;
static uint16_t *s_pixels;
static panel_lines_t s_displayed;
static uint32_t s_drawn_rows;

static bool lcd_dma_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)io;
    (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(ctx, &wake);
    return wake == pdTRUE;
}

esp_err_t front_panel_lcd_init(void)
{
    const gpio_config_t backlight = {
        .pin_bit_mask = 1ULL << LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&backlight);
    if (err != ESP_OK) return err;
#ifdef CONFIG_FRONT_LCD_BL_ACTIVE_LOW
    const int bl_on = 0;
#else
    const int bl_on = 1;
#endif
    gpio_set_level(LCD_BL, !bl_on);
    s_dma_done = xSemaphoreCreateBinary();
    s_pixels = heap_caps_malloc(LCD_WIDTH * ROW_HEIGHT * sizeof(*s_pixels),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_dma_done || !s_pixels) {
        err = ESP_ERR_NO_MEM;
        goto free_buffers;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * ROW_HEIGHT * sizeof(*s_pixels),
    };
    err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) goto free_buffers;

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_FRONT_LCD_CLOCK_MHZ * 1000000,
        .trans_queue_depth = 1,
        .on_color_trans_done = lcd_dma_done,
        .user_ctx = s_dma_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io;
    err = esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io);
    if (err != ESP_OK) goto free_bus;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_config, &s_panel);
    if (err != ESP_OK) goto free_io;
    err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
#ifdef CONFIG_FRONT_LCD_INVERT
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(s_panel, true);
#endif
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err == ESP_OK) {
        gpio_set_level(LCD_BL, bl_on);
        return ESP_OK;
    }
    esp_lcd_panel_del(s_panel);
free_io:
    esp_lcd_panel_io_del(io);
free_bus:
    spi_bus_free(SPI3_HOST);
free_buffers:
    heap_caps_free(s_pixels);
    if (s_dma_done) vSemaphoreDelete(s_dma_done);
    return err;
}

esp_err_t front_panel_lcd_show(const panel_lines_t lines)
{
    for (unsigned row = 0; row < PANEL_ROWS; ++row) {
        if ((s_drawn_rows & (1u << row)) && strcmp(lines[row], s_displayed[row]) == 0) continue;
        const uint16_t bg = __builtin_bswap16(COLOR_BG);
        const uint16_t fg = __builtin_bswap16(row == 0 || lines[row][0] == '>' ? COLOR_TITLE : COLOR_TEXT);
        for (unsigned i = 0; i < LCD_WIDTH * ROW_HEIGHT; ++i) s_pixels[i] = bg;
        for (unsigned i = 0; i < PANEL_COLS && lines[row][i]; ++i) {
            for (unsigned col = 0; col < 5; ++col) {
                const uint8_t bits = glyph(lines[row][i], col);
                const unsigned x = 6 + GLYPH_ADVANCE * i + 2 * col;
                for (unsigned y = 0; y < 7; ++y) {
                    if (!(bits & (1u << y))) continue;
                    uint16_t *p = &s_pixels[(2 * y + 1) * LCD_WIDTH + x];
                    p[0] = p[1] = p[LCD_WIDTH] = p[LCD_WIDTH + 1] = fg;
                }
            }
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, row * ROW_HEIGHT,
                                                  LCD_WIDTH, (row + 1) * ROW_HEIGHT, s_pixels);
        if (err != ESP_OK) return err;
        /* If the callback times out the caller stops drawing and leaves the
         * buffer owned by the driver, so an in-flight DMA never sees freed RAM. */
        if (xSemaphoreTake(s_dma_done, pdMS_TO_TICKS(250)) != pdTRUE) return ESP_ERR_TIMEOUT;
        memcpy(s_displayed[row], lines[row], sizeof(s_displayed[row]));
        s_drawn_rows |= 1u << row;
    }
    return ESP_OK;
}
