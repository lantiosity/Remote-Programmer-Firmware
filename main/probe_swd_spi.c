/* ESP32-S3 SWD over SPI2. Uses the IDF 6.1 LL API intentionally: short SWD
 * phases must not acquire a queue, insert dummy clocks, or run through DMA.
 * SPI0/1 (Flash/PSRAM) are never touched. Existing GPIO4/5 wiring is retained.
 */
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_cpu.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/spi_ll.h"
#include "soc/spi_periph.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc.h"
#include "probe_swd.h"
#include "probe_swd_spi.h"

static spi_dev_t *s_hw;
static int s_clk = -1, s_dio = -1;
static swd_spi_stats_t s_stats = {.alignment = -1};
static int s_alignment = -1;
static bool s_rx_delayed;
static bool s_calibrated;

static inline void io_fence(void)
{
    __asm__ volatile("memw" ::: "memory");
}

static uint32_t set_clock_at_most(uint32_t requested)
{
    /* DAP_SWJ_Clock is a maximum. The SDK's nearest-divider helper can round
     * above it (e.g. 15 -> 16 MHz); choose the fastest legal divider below it. */
    uint32_t best_div = UINT32_MAX, best_n = 2, best_pre = 1;
    for (uint32_t n = 2; n <= 64; ++n) {
        uint32_t denominator = n * requested;
        uint32_t pre = (80000000u + denominator - 1u) / denominator;
        if (!pre) pre = 1;
        if (pre > SPI_LL_MAX_PRE_DIV_NUM) continue;
        uint32_t divisor = pre * n;
        if (divisor <= best_div) {
            best_div = divisor;
            best_n = n;
            best_pre = pre;
        }
    }
    if (best_div == UINT32_MAX) return 0;
    typeof(s_hw->clock) clock = {.val = 0};
    clock.clkcnt_n = best_n - 1;
    clock.clkdiv_pre = best_pre - 1;
    clock.clkcnt_l = best_n - 1;
    clock.clkcnt_h = best_n / 2 - 1;
    s_hw->clock.val = clock.val;
    return 80000000u / best_div;
}

static void restore_gpio(bool idle_low)
{
    /* SPI mode 0 stops low. Restore low before switching the matrix; raising
     * idle-high while SWDIO=0 emits one deliberate, legal low idle clock. */
    REG_WRITE(GPIO_OUT_W1TC_REG, (1u << s_clk) | (1u << s_dio));
    esp_rom_gpio_connect_out_signal(s_clk, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(s_dio, SIG_GPIO_OUT_IDX, false, false);
    REG_WRITE(GPIO_ENABLE_W1TS_REG, (1u << s_clk) | (1u << s_dio));
    io_fence();
    if (!idle_low) {
        REG_WRITE(GPIO_OUT_W1TS_REG, 1u << s_clk);
    }
    REG_WRITE(GPIO_OUT_W1TS_REG, 1u << s_dio);
    io_fence();
}

esp_err_t swd_spi_prepare(int clk, int dio, uint32_t requested_hz)
{
    if (requested_hz < SWD_SPI_MIN_HZ) {
        s_stats.ready = false;
        s_stats.actual_hz = 0;
        s_calibrated = false;
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_hw && (!s_stats.ready || s_clk != clk || s_dio != dio)) {
        swd_spi_deinit();
    }
    if (!s_hw) {
        uint32_t saved_out = REG_READ(GPIO_OUT_REG);
        uint32_t saved_enable = REG_READ(GPIO_ENABLE_REG);
        spi_bus_config_t bus = {
            .mosi_io_num = dio, .miso_io_num = -1, .sclk_io_num = clk,
            .quadwp_io_num = -1, .quadhd_io_num = -1,
            .data4_io_num = -1, .data5_io_num = -1,
            .data6_io_num = -1, .data7_io_num = -1,
            .max_transfer_sz = 64,
        };
        esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
        if (err != ESP_OK) {
            return err;             /* Never take over another SPI2 owner. */
        }
        s_clk = clk;
        s_dio = dio;
        s_hw = spi_periph_signal[SPI2_HOST].hw;
        spi_ll_master_init(s_hw);
        spi_ll_set_clk_source(s_hw, SPI_CLK_SRC_DEFAULT);
        spi_ll_disable_int(s_hw);
        spi_ll_clear_int_stat(s_hw);
        spi_ll_master_select_cs(s_hw, 6); /* No physical CS, all six disabled. */
        spi_ll_master_set_cs_setup(s_hw, 0);
        spi_ll_master_set_cs_hold(s_hw, 0);
        spi_ll_set_half_duplex(s_hw, true);
        spi_ll_set_sio_mode(s_hw, 0); /* Manually controlled single shared pad. */
        spi_ll_set_tx_lsbfirst(s_hw, true);
        spi_ll_set_rx_lsbfirst(s_hw, true);
        spi_ll_master_set_mode(s_hw, 0);
        spi_ll_master_set_line_mode(s_hw, (spi_line_mode_t){1, 1, 1});
        spi_ll_set_data_pin_idle_level(s_hw, true);
        spi_ll_set_addr_bitlen(s_hw, 0);
        spi_ll_set_command_bitlen(s_hw, 0);
        spi_ll_set_dummy(s_hw, 0); /* SWD cannot tolerate automatic dummy bits. */
        spi_ll_set_mosi_delay(s_hw, 0, 0);
        esp_rom_gpio_connect_in_signal(dio, spi_periph_signal[SPI2_HOST].spiq_in, false);
        /* Initialization occurs before the caller's SWJ line reset. */
        const uint32_t mask = (1u << clk) | (1u << dio);
        REG_WRITE(GPIO_OUT_W1TS_REG, saved_out & mask);
        REG_WRITE(GPIO_OUT_W1TC_REG, ~saved_out & mask);
        esp_rom_gpio_connect_out_signal(clk, SIG_GPIO_OUT_IDX, false, false);
        esp_rom_gpio_connect_out_signal(dio, SIG_GPIO_OUT_IDX, false, false);
        /* These are the probe's owned pins. bus_free() disables their output
         * drivers; re-enable them before the caller's GPIO SWJ line reset. */
        (void)saved_enable;
        REG_WRITE(GPIO_ENABLE_W1TS_REG, mask);
        gpio_input_enable(dio);
        gpio_input_enable(clk);
        io_fence();
    }
    if (requested_hz < 10000) requested_hz = 10000;
    if (requested_hz > 40000000) requested_hz = 40000000;
    uint32_t actual = set_clock_at_most(requested_hz);
    if (!actual) {
        s_stats.ready = false;
        s_stats.actual_hz = 0;
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (actual != s_stats.actual_hz) {
        s_calibrated = false;
        s_rx_delayed = actual <= 20000000;
    }
    s_stats.actual_hz = actual;
    spi_ll_apply_config(s_hw);
    s_stats.ready = true;
    return ESP_OK;
}

void swd_spi_deinit(void)
{
    if (s_hw) {
        spi_ll_disable_int(s_hw);
        (void)spi_bus_free(SPI2_HOST);
        s_hw = NULL;
    }
    s_stats.ready = false;
    s_calibrated = false;
}

void swd_spi_set_alignment(int alignment)
{
    s_alignment = alignment;
    s_calibrated = false;
}

void swd_spi_reset_stats(void)
{
    bool ready = s_stats.ready;
    uint32_t hz = s_stats.actual_hz;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.ready = ready;
    s_stats.actual_hz = hz;
}

void swd_spi_get_stats(swd_spi_stats_t *stats)
{
    *stats = s_stats;
    stats->alignment = s_alignment;
    stats->rx_delayed = s_alignment < 0 ? s_rx_delayed : s_alignment != 0;
    stats->calibrated = s_calibrated;
}

static bool phase(bool output, uint32_t count, uint64_t value, uint64_t *received)
{
    if (!count) return true;
    if (count > 64) return false;
    uint8_t buffer[8];
    memcpy(buffer, &value, sizeof(buffer));
    spi_ll_clear_int_stat(s_hw);
    spi_ll_set_command_bitlen(s_hw, 0);
    spi_ll_enable_mosi(s_hw, output);
    spi_ll_enable_miso(s_hw, !output);
    if (output) {
        /* ESP32-S3 CPU FIFO TX cannot emit lengths == 1 (mod 8). Put the
         * first bit in the command phase: 33 clocks = command(1)+data(32). */
        if ((count & 7u) == 1u) {
            spi_ll_set_command_bitlen(s_hw, 1);
            spi_ll_set_command(s_hw, (uint16_t)(value & 1u), 1, true);
            value >>= 1;
            memcpy(buffer, &value, sizeof(buffer));
            spi_ll_enable_mosi(s_hw, count > 1);
            if (count > 1) {
                spi_ll_set_mosi_bitlen(s_hw, count - 1);
                spi_ll_write_buffer(s_hw, buffer, count - 1);
            }
        } else {
            spi_ll_set_mosi_bitlen(s_hw, count);
            spi_ll_write_buffer(s_hw, buffer, count);
        }
        REG_WRITE(GPIO_ENABLE_W1TS_REG, 1u << s_dio);
        s_stats.tx_bits += count;
    } else {
        spi_ll_set_miso_bitlen(s_hw, count);
        REG_WRITE(GPIO_ENABLE_W1TC_REG, 1u << s_dio);
        s_stats.rx_bits += count;
    }
    io_fence();
    spi_ll_apply_config(s_hw);
    spi_ll_user_start(s_hw);
    uint32_t started = esp_cpu_get_cycle_count();
    const uint32_t budget = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ *
        (2000u + (count * 1000000u + s_stats.actual_hz - 1u) / s_stats.actual_hz);
    while (!spi_ll_usr_is_done(s_hw) || spi_ll_get_running_cmd(s_hw)) {
        if ((uint32_t)(esp_cpu_get_cycle_count() - started) > budget) {
            ++s_stats.timeouts;
            s_stats.ready = false;
            return false;
        }
    }
    if (!output && received) {
        memset(buffer, 0, sizeof(buffer));
        spi_ll_read_buffer(s_hw, buffer, count);
        memcpy(received, buffer, sizeof(buffer));
    }
    return true;
}

uint8_t swd_spi_transfer(uint8_t request, uint32_t *data, uint32_t turnaround,
                         uint32_t idle_cycles, bool clock_idle_low)
{
    if (!s_hw || !s_stats.ready) return SWD_ACK_ERROR;
    ++s_stats.transfers;
    s_stats.last_request = request;
    const bool read = (request & 4u) != 0;
    bool delayed = s_alignment < 0 ? s_rx_delayed : s_alignment != 0;
    uint8_t ack = SWD_ACK_ERROR;
    uint64_t bits = 0;
    /* Lower CLK before mapping mode-0 SPI; a falling edge consumes no bit. */
    REG_WRITE(GPIO_OUT_W1TC_REG, 1u << s_clk);
    REG_WRITE(GPIO_ENABLE_W1TS_REG, 1u << s_clk);
    spi_ll_set_data_pin_idle_level(s_hw, true);
    spi_ll_apply_config(s_hw);
    esp_rom_gpio_connect_out_signal(s_clk, spi_periph_signal[SPI2_HOST].spiclk_out, false, false);
    esp_rom_gpio_connect_out_signal(s_dio, spi_periph_signal[SPI2_HOST].spid_out, false, false);
    io_fence();
    if (!phase(true, 8, request, NULL) ||
        !phase(false, turnaround + 3, 0, &bits)) goto io_error;
    s_stats.ack_raw = (uint32_t)bits;
    /* Calibrate only on the non-posted DP IDCODE read, not on arbitrary AP
     * reads (where WAIT is legal). The two candidate OK patterns are disjoint.
     * Keep all protocol clocks unchanged; only select which captured bits to use. */
    if (s_alignment < 0 && !s_calibrated && request == 0xA5u) {
        uint8_t standard_ack = (uint8_t)((bits >> turnaround) & 7u);
        uint8_t delayed_ack = (uint8_t)((bits >> (turnaround - 1)) & 7u);
        if (standard_ack == SWD_ACK_OK || delayed_ack == SWD_ACK_OK) {
            delayed = delayed_ack == SWD_ACK_OK;
        }
    }
    /* At lower clocks S3's fixed half-clock RX sample sees the next bit.
     * Keep that read-data bit rather than inserting an unaccounted clock. */
    ack = (uint8_t)((bits >> (delayed ? turnaround - 1 : turnaround)) & 7u);
    if (ack == SWD_ACK_OK) {
        if (read) {
            uint32_t first = (uint32_t)(bits >> (turnaround + 2)) & 1u;
            uint64_t raw = 0;
            if (!phase(false, 33, 0, &raw) ||
                !phase(false, turnaround, 0, NULL)) goto io_error;
            s_stats.rx_low = (uint32_t)raw;
            s_stats.rx_high = (uint32_t)(raw >> 32);
            uint32_t value = delayed ? ((uint32_t)raw << 1) | first : (uint32_t)raw;
            uint32_t parity = (uint32_t)(raw >> (delayed ? 31 : 32)) & 1u;
            if (data) *data = value;
            if ((__builtin_parity(value) ^ parity) != 0) {
                ++s_stats.parity_errors;
                ack = SWD_ACK_ERROR;
            } else if (s_alignment < 0 && request == 0xA5u &&
                       (value & 0xFFFu) == 0x477u) {
                s_rx_delayed = delayed;
                s_calibrated = true;
            }
        } else {
            uint32_t value = data ? *data : 0;
            uint64_t outgoing = value | ((uint64_t)__builtin_parity(value) << 32);
            if (!phase(false, turnaround, 0, NULL) ||
                !phase(true, 33, outgoing, NULL)) goto io_error;
        }
    } else if (ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT) {
        if (!phase(false, turnaround, 0, NULL)) goto io_error;
    } else {
        if (!phase(false, turnaround + 33, 0, NULL)) goto io_error;
    }
    while (idle_cycles) {
        uint32_t chunk = idle_cycles > 64 ? 64 : idle_cycles;
        if (!phase(true, chunk, 0, NULL)) goto io_error;
        idle_cycles -= chunk;
    }
    goto done;
io_error:
    ack = SWD_ACK_ERROR;
    /* Detach the stalled peripheral and finish any target-owned data phase
     * with GPIO input clocks before reclaiming SWDIO. */
    REG_WRITE(GPIO_ENABLE_W1TC_REG, 1u << s_dio);
    REG_WRITE(GPIO_OUT_W1TC_REG, 1u << s_clk);
    esp_rom_gpio_connect_out_signal(s_clk, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(s_dio, SIG_GPIO_OUT_IDX, false, false);
    io_fence();
    for (unsigned i = 0; i < 34; ++i) {
        REG_WRITE(GPIO_OUT_W1TS_REG, 1u << s_clk);
        esp_rom_delay_us(1);
        REG_WRITE(GPIO_OUT_W1TC_REG, 1u << s_clk);
        esp_rom_delay_us(1);
    }
done:
    restore_gpio(clock_idle_low);
    return ack;
}
