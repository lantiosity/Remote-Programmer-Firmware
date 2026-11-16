/**
 * @file  probe_swd.h
 * @brief  GPIO 位操作实现的 SWD 物理层（与 CMSIS-DAP 无关的最小原语）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SWD 应答（与 CMSIS-DAP 的 ACK 编码一致） */
#define SWD_ACK_OK     0x01u
#define SWD_ACK_WAIT   0x02u
#define SWD_ACK_FAULT  0x04u
#define SWD_ACK_ERROR  0x08u

/** SWJ_Pins 中的位定义（与 CMSIS-DAP 一致） */
#define SWD_PIN_SWCLK  0x01u
#define SWD_PIN_SWDIO  0x02u
#define SWD_PIN_TDI    0x04u
#define SWD_PIN_TDO    0x08u
#define SWD_PIN_NTRST  0x20u
#define SWD_PIN_NRESET 0x80u

typedef struct {
    int clk_gpio;
    int dio_gpio;
    int nrst_gpio;   /**< -1 表示未接 */
} swd_pins_t;

esp_err_t probe_swd_init(const swd_pins_t *pins);

/** 设置 SWD 请求频率（10 kHz..50 MHz）；GPIO 开销使实际频率不同。 */
void probe_swd_set_clock(uint32_t hz);
uint32_t probe_swd_get_clock(void);

typedef struct {
    uint32_t samples;
    uint32_t min_cycles;
    uint32_t max_cycles;
    uint64_t total_cycles;
} swd_phase_timing_t;

typedef struct {
    bool enabled;
    uint32_t requested_hz;
    uint32_t half_nops;
    uint32_t cpu_mhz;
    uint32_t fast_low_nops;
    bool fast_enabled;
    bool dedicated_ready;
    bool spi_enabled;
    bool spi_selected;
    uint32_t spi_hz;
    swd_phase_timing_t read;
    swd_phase_timing_t write;
    uint32_t acks[8];
    uint32_t parity_errors;
} swd_timing_stats_t;

/** 调用者持有 probe_lock；统计实际事务中 32 位数据段的 CPU 周期。
 * 包含循环与采样开销及中断暂停，不代表示波器测量或单周期峰值。 */
void probe_swd_timing_reset(bool enabled);
void probe_swd_timing_get(swd_timing_stats_t *stats);
/** 在 CPU1、线复位前准备专用 GPIO；失败时保留普通 GPIO 后端。 */
void probe_swd_prepare_fast(void);
void probe_swd_leave_fast(void);
/** -1 禁用专用 GPIO；0..32 启用并设置SWCLK 低电平期间额外 NOP 次数。 */
void probe_swd_set_fast_padding(int nops);
esp_err_t probe_swd_set_spi(bool enabled);

/** 传输一次 SWD 请求；request 为完整的 8 位请求（含起始/校验/停止/空闲位）。
 *  读操作时 *data 返回读到的数据（仅 ACK==OK 时有效）。返回 ACK。 */
uint8_t probe_swd_transfer(uint8_t request, uint32_t *data);

/** 输出 count 个时钟，同时输出 value 的对应位（LSB first），可选读回目标线 */
void probe_swd_output_sequence(uint32_t count, uint32_t value, uint32_t *read_out);

/** 输出原始位序列（LSB first，按字节展开），主要用于 DAP_SWJ_Sequence / 线复位 */
void probe_swd_output_bits(uint32_t count, const uint8_t *data);

/** 读回原始位序列 */
void probe_swd_input_bits(uint32_t count, uint8_t *data);

/** 线复位：仅输出 >=50 个高电平；所需 idle 由调用方另行输出 */
void probe_swd_line_reset(void);

/** 补 idle 个时钟（DIO 由主机驱动为低） */
void probe_swd_idle(uint32_t cycles);

/** nRESET 控制 */
void probe_swd_set_reset(bool asserted);
bool probe_swd_get_reset(void);

/** SWJ_Pins 风格的引脚读写 */
void    probe_swd_pins_write(uint8_t mask, uint8_t value);
uint8_t probe_swd_pins_read(void);

void probe_swd_delay_us(uint32_t us);

/** 电气诊断：读 SWDIO/SWCLK 在内部上拉、下拉时的电平，以及 nRESET 电平 */
void probe_swd_diag(int *dio_up, int *dio_down, int *clk_up, int *clk_down, int *nrst);

/** 电压诊断：SWDIO(内部下拉时)/SWCLK(内部上拉时)/nRESET 的电压，单位 mV */
void probe_swd_diag_mv(int *dio_mv_down, int *clk_mv_up, int *nrst_mv);

/** 请求后额外插入的 turnaround 时钟数（0 = DAPLink 风格，1 = 规范风格） */
void probe_swd_set_turnaround(uint32_t cycles);

/** 验证 SWDIO 释放(tri-state)是否生效：bit0=驱动高时电平，bit1=释放+下拉后电平 */
int probe_swd_release_test(void);

/** 测 nRESET 线上升时间（µs）；0xFFFFFFFF=未配置，0xFFFFFFFE=一直不升高 */
uint32_t probe_swd_nrst_rise_us(void);

/** 交换 SWCLK/SWDIO 角色 */
void probe_swd_swap_pins(void);

/** SWCLK 空闲电平：v=1 时空闲拉低（经典 SWD 规范） */
void probe_swd_set_clk_idle_low(int v);

/** 传输末尾额外 idle 时钟数 */
void probe_swd_set_idle(uint32_t cycles);

#ifdef __cplusplus
}
#endif
