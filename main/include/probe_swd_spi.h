#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SWD_SPI_MIN_HZ 78125u /* 80 MHz / (16 predivider * 64 counter) */

typedef struct {
    bool ready;
    uint32_t actual_hz;
    uint32_t transfers;
    uint32_t timeouts;
    uint32_t parity_errors;
    uint32_t last_request;
    uint32_t ack_raw;
    uint32_t rx_low;
    uint32_t rx_high;
    uint64_t tx_bits;
    uint64_t rx_bits;
    int alignment;                 /* -1 auto, 0 standard, 1 delayed */
    bool rx_delayed;
    bool calibrated;
} swd_spi_stats_t;

/* SPI2 exclusively reserved by spi_bus_initialize, CPU polling / no DMA.
 * Caller owns probe_lock and has detached the dedicated GPIO output route. */
esp_err_t swd_spi_prepare(int clk, int dio, uint32_t requested_hz);
void swd_spi_deinit(void);
void swd_spi_set_alignment(int alignment);
void swd_spi_reset_stats(void);
void swd_spi_get_stats(swd_spi_stats_t *stats);
uint8_t swd_spi_transfer(uint8_t request, uint32_t *data, uint32_t turnaround,
                         uint32_t idle_cycles, bool clock_idle_low);
