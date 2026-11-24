#pragma once

#include <stddef.h>
#include <stdint.h>

enum { PANEL_WIDTH = 240, PANEL_HEIGHT = 320, PANEL_STRIP = 16 };
enum { PANEL_DRAW_LIMIT = 48, PANEL_TEXT_BYTES = 84 };
typedef enum { PANEL_RECT, PANEL_BODY, PANEL_TITLE } panel_draw_kind_t;

typedef struct {
    uint16_t x, y, width, height, color;
    panel_draw_kind_t kind;
    char text[PANEL_TEXT_BYTES];
} panel_draw_t;

typedef struct {
    uint16_t background;
    size_t count;
    panel_draw_t items[PANEL_DRAW_LIMIT];
} panel_scene_t;

#define PANEL_RGB(r, g, b) ((uint16_t)(((r) & 0xf8) << 8 | ((g) & 0xfc) << 3 | (b) >> 3))

void panel_rect(panel_scene_t *scene, unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color);
void panel_text(panel_scene_t *scene, unsigned x, unsigned y, unsigned width, uint16_t color,
                panel_draw_kind_t kind, const char *text);
void panel_render_strip(const panel_scene_t *scene, unsigned y, uint16_t *pixels, const uint8_t *font);
