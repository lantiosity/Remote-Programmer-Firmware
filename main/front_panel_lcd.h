#pragma once

#include "esp_err.h"
#include "panel_scene.h"

esp_err_t front_panel_lcd_init(void);
esp_err_t front_panel_lcd_show(const panel_scene_t *scene);
