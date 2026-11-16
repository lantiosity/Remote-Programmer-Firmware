/**
 * @file  console_cmds.h
 * @brief  UART 控制台（esp_console）：wifi / status / swd / scan / rst / baud。
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t console_start(void);

#ifdef __cplusplus
}
#endif
