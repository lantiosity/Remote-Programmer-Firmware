/**
 * @file  probe_core.h
 * @brief  Debug Port / Access Port 语义层：DP、AP、MEM-AP 内存访问、复位。
 *
 * 这一层同时服务两条链路：
 *   - CMSIS-DAP 命令层（USB HID，给 Keil / pyOCD / OpenOCD 用）
 *   - pyOCD remote 探针 JSON 协议（WiFi TCP / BLE）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "probe_swd.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 传输结果（映射到 pyOCD JSON status / CMSIS-DAP ACK） */
typedef enum {
    PROBE_OK      = 0,
    PROBE_ERR_WAIT,      /**< 目标一直返回 WAIT */
    PROBE_ERR_FAULT,     /**< 目标返回 FAULT */
    PROBE_ERR_PARITY,    /**< 校验错误/无应答 */
    PROBE_ERR_ARG,       /**< 参数错误 */
    PROBE_ERR_IO,        /**< 底层 IO 错误 */
} probe_status_t;

typedef struct {
    int      clk_gpio;
    int      dio_gpio;
    int      nrst_gpio;
    uint32_t clock_hz;
} probe_core_cfg_t;

esp_err_t probe_init(const probe_core_cfg_t *cfg);

/* ---------- 连接管理 ---------- */
esp_err_t     probe_connect(void);       /**< 建立 SWD 连接（线复位 + 读 IDCODE 自检） */
esp_err_t     probe_disconnect(void);
bool          probe_is_connected(void);
uint32_t      probe_get_clock(void);
void          probe_set_clock(uint32_t hz);

/* ---------- 传输配置 ---------- */
void probe_set_idle_cycles(uint8_t cycles);
void probe_set_wait_retry(uint32_t retry);
void probe_set_match_retry(uint32_t retry);

/* ---------- 原始传输 ---------- */
uint8_t probe_transfer(uint8_t request, uint32_t *data);
uint8_t probe_transfer_block(uint8_t request, uint32_t *data, uint32_t count, uint32_t *done);

/* ---------- DP / AP 快捷访问（AP 读自动做 posted read 回读） ---------- */
probe_status_t probe_dp_read(uint32_t addr, uint32_t *value);
probe_status_t probe_dp_write(uint32_t addr, uint32_t value);
probe_status_t probe_ap_read(uint32_t addr, uint32_t *value);
probe_status_t probe_ap_write(uint32_t addr, uint32_t value);
probe_status_t probe_ap_read_multiple(uint32_t addr, uint32_t *values, uint32_t count);
probe_status_t probe_ap_write_multiple(uint32_t addr, const uint32_t *values, uint32_t count);

/* ---------- 序列 ---------- */
void probe_swj_sequence(uint32_t bit_count, const uint8_t *data);
void probe_swj_sequence_u64(uint32_t bit_count, uint64_t value);
void probe_pins_write(uint8_t mask, uint8_t value);
uint8_t probe_pins_read(void);
void probe_delay_us(uint32_t us);

/* ---------- 复位 ---------- */
void probe_assert_reset(bool asserted);
bool probe_is_reset_asserted(void);
probe_status_t probe_reset_target(void);

/* ---------- MEM-AP 内存访问 ---------- */
probe_status_t probe_mem_select_ap(uint32_t apsel, uint32_t bank);
probe_status_t probe_mem_read(uint32_t addr, uint32_t size, uint32_t *value);
probe_status_t probe_mem_write(uint32_t addr, uint32_t size, uint32_t value);
probe_status_t probe_mem_read_block32(uint32_t addr, uint32_t *data, uint32_t words);
probe_status_t probe_mem_write_block32(uint32_t addr, const uint32_t *data, uint32_t words);
probe_status_t probe_mem_read_block8(uint32_t addr, uint8_t *data, uint32_t bytes);
probe_status_t probe_mem_write_block8(uint32_t addr, const uint8_t *data, uint32_t bytes);
probe_status_t probe_mem_flush(void);

/** 最近一次 DP IDCODE（连接时缓存） */
uint32_t probe_idcode(void);

/** 电气诊断（见 probe_swd.h） */
void probe_diag(int *dio_up, int *dio_down, int *clk_up, int *clk_down, int *nrst);
void probe_diag_mv(int *dio_mv_down, int *clk_mv_up, int *nrst_mv);
/** SWD 总线互斥（一次性事务要整体加锁） */
void probe_lock(void);
void probe_unlock(void);

/** 交换 SWCLK/SWDIO 角色 */
void probe_swap_pins(void);

/** 请求后的额外 turnaround 时钟数（0/1 两种风格） */
void probe_set_turnaround(uint32_t cycles);

#ifdef __cplusplus
}
#endif
