#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define WEB_CONFIG_CODE_LEN 12

esp_err_t web_config_start(void);
bool web_config_arm(void);
void web_config_close(void);
/* For physical interfaces only. Never expose the setup code over HTTP. */
bool web_config_get_code(char code[WEB_CONFIG_CODE_LEN + 1]);
