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

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_dma_done;
static uint16_t *s_pixels;
static uint16_t *s_displayed;
extern const uint8_t panel_font[] asm("_binary_panel_font_bin_start");
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
    s_pixels = heap_caps_malloc(PANEL_WIDTH * PANEL_STRIP * sizeof(*s_pixels),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_displayed = heap_caps_malloc(PANEL_WIDTH * PANEL_HEIGHT * sizeof(*s_displayed),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dma_done || !s_pixels || !s_displayed) {
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
        .max_transfer_sz = PANEL_WIDTH * PANEL_STRIP * sizeof(*s_pixels),
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
    heap_caps_free(s_displayed);
    heap_caps_free(s_pixels);
    if (s_dma_done) vSemaphoreDelete(s_dma_done);
    return err;
}

esp_err_t front_panel_lcd_show(const panel_scene_t *scene)
{
    const size_t count = PANEL_WIDTH * PANEL_STRIP;
    for (unsigned y = 0; y < PANEL_HEIGHT; y += PANEL_STRIP) {
        const uint32_t strip = 1u << (y / PANEL_STRIP);
        uint16_t *previous = s_displayed + y * PANEL_WIDTH;
        panel_render_strip(scene, y, s_pixels, panel_font);
        if ((s_drawn_rows & strip) && memcmp(previous, s_pixels, count * sizeof(*s_pixels)) == 0)
            continue;
        memcpy(previous, s_pixels, count * sizeof(*s_pixels));
        for (unsigned i = 0; i < count; ++i) s_pixels[i] = __builtin_bswap16(s_pixels[i]);
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, PANEL_WIDTH, y + PANEL_STRIP, s_pixels);
        if (err != ESP_OK) return err;
        /* Leave the buffer owned by the driver if the callback times out. */
        if (xSemaphoreTake(s_dma_done, pdMS_TO_TICKS(250)) != pdTRUE) return ESP_ERR_TIMEOUT;
        s_drawn_rows |= strip;
    }
    return ESP_OK;
}
