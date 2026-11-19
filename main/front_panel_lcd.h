#pragma once

#include "esp_err.h"

enum { PANEL_ROWS = 20, PANEL_COLS = 19 };
typedef char panel_lines_t[PANEL_ROWS][PANEL_COLS + 1];

esp_err_t front_panel_lcd_init(void);
esp_err_t front_panel_lcd_show(const panel_lines_t lines);
