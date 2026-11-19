#include <stdbool.h>
#include <string.h>

#include "panel_scene.h"

static unsigned min(unsigned a, unsigned b)
{
    return a < b ? a : b;
}

void panel_rect(panel_scene_t *scene, unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
    if (!w || !h || x >= PANEL_WIDTH || y >= PANEL_HEIGHT || scene->count == PANEL_DRAW_LIMIT) return;
    scene->items[scene->count++] = (panel_draw_t){
        .x = x, .y = y, .width = min(w, PANEL_WIDTH - x), .height = min(h, PANEL_HEIGHT - y),
        .color = color, .kind = PANEL_RECT,
    };
}

void panel_text(panel_scene_t *scene, unsigned x, unsigned y, unsigned width, uint16_t color,
                panel_draw_kind_t kind, const char *text)
{
    if (!*text || !width || x >= PANEL_WIDTH || y >= PANEL_HEIGHT || scene->count == PANEL_DRAW_LIMIT) return;
    const unsigned height = kind == PANEL_TITLE ? 24 : 16;
    panel_draw_t *item = &scene->items[scene->count++];
    *item = (panel_draw_t){.x = x, .y = y, .width = min(width, PANEL_WIDTH - x), .height = height,
                          .color = color, .kind = kind};
    size_t bytes = strlen(text);
    if (bytes >= sizeof(item->text)) {
        bytes = sizeof(item->text) - 1;
        while (((unsigned char)text[bytes] & 0xc0) == 0x80) --bytes;
    }
    memcpy(item->text, text, bytes);
}

static uint32_t next_char(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t ch = *p++;
    unsigned extra = ch < 0x80 ? 0 : ch < 0xe0 ? 1 : ch < 0xf0 ? 2 : 3;
    if (extra) {
        if (ch < 0xc2 || ch > 0xf4) {
            *text = (const char *)p;
            return '?';
        }
        ch &= (1u << (6 - extra)) - 1;
        for (unsigned i = 0; i < extra; ++i) {
            if ((*p & 0xc0) != 0x80) {
                *text = (const char *)p;
                return '?';
            }
            ch = (ch << 6) | (*p++ & 0x3f);
        }
    }
    *text = (const char *)p;
    return ch;
}

static unsigned advance(uint32_t ch, unsigned size)
{
    return ch < 0x80 ? (size == 24 ? 15 : 10) : size;
}

static unsigned le16(const uint8_t *p)
{
    return p[0] | ((unsigned)p[1] << 8);
}

static const uint8_t *glyph(const uint8_t *font, unsigned size, uint32_t ch)
{
    unsigned count = le16(font);
    const uint8_t *table = font + 4;
    if (size == 24) {
        table += count * (2 + 16 * 16 / 4);
        count = le16(font + 2);
    }
    const unsigned record = 2 + size * size / 4;
    unsigned first = 0, last = count;
    while (first < last) {
        unsigned index = first + (last - first) / 2;
        unsigned code = le16(table + index * record);
        if (code == ch) return table + index * record + 2;
        if (code < ch) first = index + 1;
        else last = index;
    }
    return ch == '?' ? NULL : glyph(font, size, '?');
}

static uint16_t blend(uint16_t bg, uint16_t fg, unsigned a)
{
    if (a == 3) return fg;
    unsigned b = 3 - a;
    unsigned red = ((bg >> 11) * b + (fg >> 11) * a + 1) / 3;
    unsigned green = (((bg >> 5) & 63) * b + ((fg >> 5) & 63) * a + 1) / 3;
    unsigned blue = ((bg & 31) * b + (fg & 31) * a + 1) / 3;
    return (red << 11) | (green << 5) | blue;
}

static void draw_text(const panel_draw_t *item, unsigned y, uint16_t *pixels, const uint8_t *font)
{
    const unsigned size = item->height;
    const char *p = item->text;
    unsigned total = 0;
    while (*p) total += advance(next_char(&p), size);
    p = item->text;
    unsigned used = 0;
    const bool clipped = total > item->width;
    while (*p && used < item->width) {
        uint32_t ch = next_char(&p);
        unsigned width = advance(ch, size);
        if (clipped && used + width + size > item->width) {
            ch = 0x2026;
            width = min(size, item->width - used);
            p = "";
        }
        const uint8_t *bits = glyph(font, size, ch);
        if (bits) {
            const unsigned top = y > item->y ? y : item->y;
            const unsigned bottom = min(item->y + size, y + PANEL_STRIP);
            for (unsigned py = top; py < bottom; ++py) {
                uint16_t *dest = pixels + (py - y) * PANEL_WIDTH + item->x + used;
                unsigned cell = (py - item->y) * size;
                for (unsigned x = 0; x < width; ++x, ++cell) {
                    unsigned alpha = (bits[cell / 4] >> ((cell % 4) * 2)) & 3;
                    if (alpha) dest[x] = blend(dest[x], item->color, alpha);
                }
            }
        }
        used += width;
    }
}

void panel_render_strip(const panel_scene_t *scene, unsigned y, uint16_t *pixels, const uint8_t *font)
{
    for (unsigned i = 0; i < PANEL_WIDTH * PANEL_STRIP; ++i) pixels[i] = scene->background;
    for (size_t i = 0; i < scene->count; ++i) {
        const panel_draw_t *item = &scene->items[i];
        if (item->y >= y + PANEL_STRIP || item->y + item->height <= y) continue;
        if (item->kind != PANEL_RECT) {
            draw_text(item, y, pixels, font);
            continue;
        }
        const unsigned top = item->y > y ? item->y : y;
        const unsigned bottom = min(item->y + item->height, y + PANEL_STRIP);
        for (unsigned py = top; py < bottom; ++py) {
            uint16_t *dest = pixels + (py - y) * PANEL_WIDTH + item->x;
            for (unsigned x = 0; x < item->width; ++x) dest[x] = item->color;
        }
    }
}
