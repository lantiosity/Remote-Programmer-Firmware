/**
 * @file  swd_emu.h
 * @brief  内置 SWD 目标仿真器（中断驱动，自检用）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t swd_emu_start(void);
void      swd_emu_stop(void);
bool      swd_emu_is_running(void);

/** 取统计：收到的 8 位组总数、合法请求数、完成的读事务数；buf 返回最近 16 个组 */
void swd_emu_dump(uint8_t *buf, uint32_t buf_len, uint32_t *total,
                  uint32_t *valid, uint32_t *reads);

#ifdef __cplusplus
}
#endif
