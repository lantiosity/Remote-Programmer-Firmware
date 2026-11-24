#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "panel_scene.h"

enum { PANEL_FILES_VISIBLE = 4 };
typedef enum { PANEL_HOME, PANEL_FILES, PANEL_SETUP, PANEL_CONFIRM,
               PANEL_WORKING, PANEL_RESULT, PANEL_STATUS } panel_page_t;
typedef enum { PANEL_CARD_EMPTY, PANEL_CARD_LOADING,
               PANEL_CARD_READY, PANEL_CARD_ERROR } panel_card_t;

typedef struct {
    panel_page_t page;
    panel_card_t card;
    const char *version, *ip, *web_code;
    const char *files[PANEL_FILES_VISIBLE];
    unsigned page_index, pages, selected;
    bool more, directory_error;
    const char *filename;
    size_t size;
    uint32_t address, last_address;
    bool saved, reset_run, saving, success;
    const char *stage, *detail;
    unsigned percent;
    unsigned key_counts[6];
} panel_view_t;

void front_panel_compose(panel_scene_t *scene, const panel_view_t *view);
