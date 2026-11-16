/**
 * @file  probe_swd.c
 * @brief  GPIO 位操作 SWD 物理层（ESP32-S3）
 *
 * 时序严格对照 ARM 官方 CMSIS-DAP 的 SW_DP.c 实现：
 *
 *   SW_CLOCK_CYCLE():   CLK 低 -> 延时 -> CLK 高 -> 延时
 *   SW_WRITE_BIT(bit):  置数据 -> CLK 低 -> 延时 -> CLK 高 -> 延时
 *   SW_READ_BIT():      CLK 低 -> 延时 -> 采样 -> CLK 高 -> 延时
 *
 *   请求 8 位 -> 释放 SWDIO -> turnaround(默认 1) 个时钟 -> 采样 3 位 ACK
 *   读: 32 位数据 + 校验位 -> turnaround -> 收回总线 -> idle
 *   写: turnaround -> 收回总线 -> 32 位数据 + 校验位 -> idle
 *
 * 参考：huming2207/swd-esp（ARM DAPLink SW_DP.c 的 ESP32 移植）
 */
#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/dedic_gpio.h"
#include "esp_cpu.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "hal/dedic_gpio_periph.h"
#include "freertos/FreeRTOS.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc_caps.h"
#include "soc/soc.h"

#include "probe_swd.h"
#include "probe_swd_spi.h"

static const char *TAG = "swd";

static int      s_clk = -1;
static int      s_dio = -1;
static int      s_nrst = -1;
static uint32_t s_clk_mask = 0;
static uint32_t s_dio_mask = 0;
static uint32_t s_half_nops = 20;
static uint32_t s_clock_hz = 1000000;
static uint32_t s_turn_cycles = 1;   /* DAPLink 默认 1 */
static uint32_t s_idle_cycles = 0;
static int      s_clk_idle_low = 0;   /* 1 = 空闲时 SWCLK 拉低 */
static bool     s_timing_enabled;
static swd_timing_stats_t s_timing;
static dedic_gpio_bundle_handle_t s_fast_out;
static dedic_gpio_bundle_handle_t s_fast_in;
static uint32_t s_fast_clk_mask;
static uint32_t s_fast_dio_mask;
static uint32_t s_fast_in_mask;
static int s_fast_clk_signal;
static int s_fast_dio_signal;
static bool s_fast_enabled = true;
static bool s_fast_active;
static uint32_t s_fast_low_nops = 0; /* 固定高脉冲保护仍保留，非零宽度时钟 */
static bool s_spi_selected;

/* 专用 GPIO 只绑定 CPU1；在连接期间保持路由，避免逐事务切换 matrix。
 * 切换频率、断开或电气诊断时先同步输出锁存器，再恢复普通 GPIO。 */
static void fast_restore_gpio(void)
{
    uint32_t out = dedic_gpio_cpu_ll_read_out();
    uint32_t hi = ((out & s_fast_clk_mask) ? s_clk_mask : 0) |
                  ((out & s_fast_dio_mask) ? s_dio_mask : 0);
    REG_WRITE(GPIO_OUT_W1TS_REG, hi);
    REG_WRITE(GPIO_OUT_W1TC_REG, (s_clk_mask | s_dio_mask) & ~hi);
    esp_rom_gpio_connect_out_signal(s_clk, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(s_dio, SIG_GPIO_OUT_IDX, false, false);
}

void probe_swd_leave_fast(void)
{
    if (s_fast_active) {
        configASSERT(esp_cpu_get_core_id() == 1);
        fast_restore_gpio();
        __asm__ volatile("memw" ::: "memory");
        s_fast_active = false;
    }
}

static void fast_release(void)
{
    if (s_fast_out) {
        (void)dedic_gpio_del_bundle(s_fast_out);
        s_fast_out = NULL;
    }
    if (s_fast_in) {
        (void)dedic_gpio_del_bundle(s_fast_in);
        s_fast_in = NULL;
    }
}

void probe_swd_prepare_fast(void)
{
    if (s_spi_selected) {
        probe_swd_leave_fast();
        esp_err_t err = swd_spi_prepare(s_clk, s_dio, s_clock_hz);
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
            ESP_LOGE(TAG, "SPI2 prepare failed: %s", esp_err_to_name(err));
        return;
    }
    if (!s_fast_enabled || s_fast_out || esp_cpu_get_core_id() != 1) {
        return;
    }
    uint32_t old_out = REG_READ(GPIO_OUT_REG);
    uint32_t old_enable = REG_READ(GPIO_ENABLE_REG);
    const int pins[] = {s_clk, s_dio};
    dedic_gpio_bundle_config_t output = {
        .gpio_array = pins, .array_size = 2, .flags = {.out_en = 1},
    };
    dedic_gpio_bundle_config_t input = {
        .gpio_array = &s_dio, .array_size = 1, .flags = {.in_en = 1},
    };
    esp_err_t err = dedic_gpio_new_bundle(&output, &s_fast_out);
    uint32_t offset = 0;
    if (err == ESP_OK) {
        err = dedic_gpio_get_out_offset(s_fast_out, &offset);
        s_fast_clk_mask = 1u << offset;
        s_fast_dio_mask = 2u << offset;
        s_fast_clk_signal = dedic_gpio_periph_signals.cores[1].out_sig_per_channel[offset];
        s_fast_dio_signal = dedic_gpio_periph_signals.cores[1].out_sig_per_channel[offset + 1];
    }
    if (err == ESP_OK) {
        err = dedic_gpio_new_bundle(&input, &s_fast_in);
    }
    if (err == ESP_OK) {
        err = dedic_gpio_get_in_mask(s_fast_in, &s_fast_in_mask);
    }
    /* 只在随后的 SWJ 线复位之前初始化；恢复普通 GPIO 的电平与方向。 */
    if (err != ESP_OK) {
        fast_release();
        ESP_LOGW(TAG, "Dedicated GPIO unavailable: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Dedicated GPIO ready on CPU1, low-phase NOPs=%" PRIu32,
                 s_fast_low_nops);
    }
    REG_WRITE(GPIO_OUT_W1TS_REG, old_out & (s_clk_mask | s_dio_mask));
    REG_WRITE(GPIO_OUT_W1TC_REG, ~old_out & (s_clk_mask | s_dio_mask));
    esp_rom_gpio_connect_out_signal(s_clk, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(s_dio, SIG_GPIO_OUT_IDX, false, false);
    REG_WRITE(GPIO_ENABLE_W1TS_REG, old_enable & (s_clk_mask | s_dio_mask));
    REG_WRITE(GPIO_ENABLE_W1TC_REG, ~old_enable & (s_clk_mask | s_dio_mask));
}

void probe_swd_set_fast_padding(int nops)
{
    if (nops < 0) {
        probe_swd_leave_fast();
        s_fast_enabled = false;
    } else if (nops <= 32) {
        s_fast_low_nops = (uint32_t)nops;
        s_fast_enabled = true;
    }
}

esp_err_t probe_swd_set_spi(bool enabled)
{
    probe_swd_leave_fast();
    if (enabled) {
        esp_err_t err = swd_spi_prepare(s_clk, s_dio, s_clock_hz);
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) return err;
    }
    s_spi_selected = enabled;
    return ESP_OK;
}

static void timing_sample(swd_phase_timing_t *phase, uint32_t cycles)
{
    if (phase->samples == 0 || cycles < phase->min_cycles) {
        phase->min_cycles = cycles;
    }
    if (cycles > phase->max_cycles) {
        phase->max_cycles = cycles;
    }
    ++phase->samples;
    phase->total_cycles += cycles;
}

/* ---------------- 底层位操作 ---------------- */

static inline void clk_hi(void)
{
    if (s_fast_active) dedic_gpio_cpu_ll_write_mask(s_fast_clk_mask, s_fast_clk_mask);
    else REG_WRITE(GPIO_OUT_W1TS_REG, s_clk_mask);
}
static inline void clk_lo(void)
{
    if (s_fast_active) dedic_gpio_cpu_ll_write_mask(s_fast_clk_mask, 0);
    else REG_WRITE(GPIO_OUT_W1TC_REG, s_clk_mask);
}
static inline void dio_hi(void)
{
    if (s_fast_active) dedic_gpio_cpu_ll_write_mask(s_fast_dio_mask, s_fast_dio_mask);
    else REG_WRITE(GPIO_OUT_W1TS_REG, s_dio_mask);
}
static inline void dio_lo(void)
{
    if (s_fast_active) dedic_gpio_cpu_ll_write_mask(s_fast_dio_mask, 0);
    else REG_WRITE(GPIO_OUT_W1TC_REG, s_dio_mask);
}
static inline int dio_get(void)
{
    return s_fast_active ? ((dedic_gpio_cpu_ll_read_in() & s_fast_in_mask) != 0)
                         : ((REG_READ(GPIO_IN_REG) & s_dio_mask) != 0);
}
static inline void dio_out(void)
{
    REG_WRITE(GPIO_ENABLE_W1TS_REG, s_dio_mask);
    __asm__ volatile("memw" ::: "memory");
}
static inline void dio_in(void)
{
    REG_WRITE(GPIO_ENABLE_W1TC_REG, s_dio_mask);
    __asm__ volatile("memw" ::: "memory");
}

static inline void clk_idle(void)
{
    if (s_clk_idle_low) {
        clk_lo();
    } else {
        clk_hi();
    }
}

static inline void delay_half(void)
{
    for (uint32_t i = 0; i < s_half_nops; ++i) {
        __asm__ volatile("nop");
    }
}

/* 只打一个时钟（与 DAPLink 的 SW_CLOCK_CYCLE 一致） */
static inline void clk_cycle(void)
{
    clk_lo();
    delay_half();
    clk_hi();
    delay_half();
}

/* 输出一位：数据在时钟高电平期间建立，随后 低->高 */
static inline void wr_bit(int bit)
{
    if (bit) {
        dio_hi();
    } else {
        dio_lo();
    }
    clk_lo();
    delay_half();
    clk_hi();
    delay_half();
}

/* 读一位：时钟拉低、延时后采样，再拉高（与 SW_READ_BIT 一致） */
static inline int rd_bit(void)
{
    clk_lo();
    delay_half();
    int bit = dio_get();
    clk_hi();
    delay_half();
    return bit;
}

static void update_timing(uint32_t hz)
{
    if (hz < 10000) {
        hz = 10000;
    }
    if (hz > 50000000) {
        hz = 50000000;
    }
    s_clock_hz = hz;
    uint32_t half_cycles = (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u) / (2u * hz);
    uint32_t nops = half_cycles / 4u;
    /* 高频允许零个额外 nop；GPIO 访问和位循环仍有不可消除的开销。
     * s_clock_hz 是请求设置，不是测得的 SWCLK 频率。 */
    if (nops > 2000) {
        nops = 2000;
    }
    s_half_nops = nops;
}

/* ---------------- 对外接口 ---------------- */

esp_err_t probe_swd_init(const swd_pins_t *pins)
{
    if (pins == NULL || pins->clk_gpio < 0 || pins->dio_gpio < 0 ||
        pins->clk_gpio >= SOC_GPIO_PIN_COUNT || pins->dio_gpio >= SOC_GPIO_PIN_COUNT ||
        (pins->nrst_gpio >= 0 && pins->nrst_gpio >= SOC_GPIO_PIN_COUNT)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_fast_out && esp_cpu_get_core_id() != 1) {
        return ESP_ERR_INVALID_STATE;
    }
    probe_swd_leave_fast();
    swd_spi_deinit();
    if (s_fast_out) {
        fast_release();
    }
    s_clk = pins->clk_gpio;
    s_dio = pins->dio_gpio;
    s_nrst = pins->nrst_gpio;
    s_clk_mask = 1u << s_clk;
    s_dio_mask = 1u << s_dio;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_clk),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t dio = {
        .pin_bit_mask = (1ULL << s_dio),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&dio);
    if (err != ESP_OK) {
        return err;
    }

    if (s_nrst >= 0) {
        gpio_config_t rst = {
            .pin_bit_mask = (1ULL << s_nrst),
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&rst);
        if (err != ESP_OK) {
            return err;
        }
        gpio_set_level(s_nrst, 1);
    }

    /* 默认与 DAPLink 一致：空闲时 SWCLK 高、SWDIO 高 */
    clk_idle();
    dio_hi();
    dio_out();
    update_timing(1000000);

    ESP_LOGI(TAG, "SWCLK=GPIO%d SWDIO=GPIO%d nRESET=%d", s_clk, s_dio, s_nrst);
    return ESP_OK;
}

void probe_swd_set_clock(uint32_t hz)
{
    probe_swd_leave_fast();
    update_timing(hz);
    if (s_spi_selected) {
        esp_err_t err = swd_spi_prepare(s_clk, s_dio, s_clock_hz);
        if (err == ESP_ERR_NOT_SUPPORTED)
            ESP_LOGI(TAG, "SPI2 low-clock request uses GPIO (%" PRIu32 " Hz)", s_clock_hz);
        else if (err != ESP_OK)
            ESP_LOGE(TAG, "SPI2 clock failed: %s", esp_err_to_name(err));
    }
}

uint32_t probe_swd_get_clock(void)
{
    return s_clock_hz;
}

void probe_swd_timing_reset(bool enabled)
{
    memset(&s_timing, 0, sizeof(s_timing));
    s_timing_enabled = enabled;
    swd_spi_reset_stats();
}

void probe_swd_timing_get(swd_timing_stats_t *stats)
{
    *stats = s_timing;
    stats->enabled = s_timing_enabled;
    stats->requested_hz = s_clock_hz;
    stats->half_nops = s_half_nops;
    stats->cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    stats->fast_low_nops = s_fast_low_nops;
    stats->fast_enabled = s_fast_enabled;
    stats->dedicated_ready = s_fast_out != NULL;
    swd_spi_stats_t spi;
    swd_spi_get_stats(&spi);
    stats->spi_selected = s_spi_selected;
    stats->spi_enabled = s_spi_selected && spi.ready;
    stats->spi_hz = spi.actual_hz;
}

void probe_swd_set_turnaround(uint32_t cycles)
{
    if (cycles < 1) {
        cycles = 1;
    }
    if (cycles > 4) {
        cycles = 4;
    }
    s_turn_cycles = cycles;
}

void probe_swd_set_clk_idle_low(int v)
{
    s_clk_idle_low = v ? 1 : 0;
}

void probe_swd_set_idle(uint32_t cycles)
{
    if (cycles > 255) {
        cycles = 255;
    }
    s_idle_cycles = cycles;
}

/** 交换 SWCLK/SWDIO 的引脚角色（用于自动识别目标侧接反） */
void probe_swd_swap_pins(void)
{
    probe_swd_leave_fast();
    int t = s_clk;
    s_clk = s_dio;
    s_dio = t;
    s_clk_mask = 1u << s_clk;
    s_dio_mask = 1u << s_dio;

    const swd_pins_t pins = {
        .clk_gpio = s_clk,
        .dio_gpio = s_dio,
        .nrst_gpio = s_nrst,
    };
    const uint32_t hz = s_clock_hz;
    const uint32_t turn = s_turn_cycles;
    const uint32_t idle = s_idle_cycles;
    (void)probe_swd_init(&pins);
    update_timing(hz);
    s_turn_cycles = turn;
    s_idle_cycles = idle;
}

static uint8_t transfer_regular(uint8_t request, uint32_t *data)
{
#define SWD_WRITE_BIT(bit) wr_bit(bit)
#define SWD_READ_BIT() rd_bit()
#define SWD_CLOCK_CYCLE() clk_cycle()
#define SWD_DIO_HIGH() dio_hi()
#define SWD_DIO_LOW() dio_lo()
#define SWD_CLOCK_IDLE() clk_idle()
#include "probe_swd_transfer.inc"
#undef SWD_WRITE_BIT
#undef SWD_READ_BIT
#undef SWD_CLOCK_CYCLE
#undef SWD_DIO_HIGH
#undef SWD_DIO_LOW
#undef SWD_CLOCK_IDLE
}

static inline __attribute__((always_inline)) void fast_delay(uint32_t nops)
{
    for (uint32_t i = 0; i < nops; ++i) {
        __asm__ volatile("nop");
    }
}

static inline __attribute__((always_inline)) void fast_cycle(uint32_t clk, uint32_t nops)
{
    dedic_gpio_cpu_ll_write_mask(clk, 0);
    fast_delay(nops);
    dedic_gpio_cpu_ll_write_mask(clk, clk);
    /* ACK reads can be adjacent after inlining. Guarantee a high pulse even
     * when the caller has no data/loop work between consecutive clock edges. */
    __asm__ volatile("nop\nnop\nnop\nnop");
}

static inline __attribute__((always_inline)) void fast_write_bit(int bit, uint32_t clk,
                                                               uint32_t dio, uint32_t nops)
{
    dedic_gpio_cpu_ll_write_mask(dio, bit ? dio : 0);
    fast_cycle(clk, nops);
}

static inline __attribute__((always_inline)) int fast_read_bit(uint32_t clk, uint32_t input,
                                                             uint32_t nops)
{
    dedic_gpio_cpu_ll_write_mask(clk, 0);
    fast_delay(nops);
    int bit = (dedic_gpio_cpu_ll_read_in() & input) != 0;
    dedic_gpio_cpu_ll_write_mask(clk, clk);
    /* ACK reads can be adjacent after inlining. Guarantee a high pulse even
     * when the caller has no data/loop work between consecutive clock edges. */
    __asm__ volatile("nop\nnop\nnop\nnop");
    return bit;
}

static uint8_t IRAM_ATTR __attribute__((noinline)) transfer_dedicated(uint8_t request, uint32_t *data)
{
    const uint32_t clk = s_fast_clk_mask;
    const uint32_t dio = s_fast_dio_mask;
    const uint32_t input = s_fast_in_mask;
    const uint32_t nops = s_fast_low_nops;
#define SWD_WRITE_BIT(bit) fast_write_bit((bit), clk, dio, nops)
#define SWD_READ_BIT() fast_read_bit(clk, input, nops)
#define SWD_CLOCK_CYCLE() fast_cycle(clk, nops)
#define SWD_DIO_HIGH() dedic_gpio_cpu_ll_write_mask(dio, dio)
#define SWD_DIO_LOW() dedic_gpio_cpu_ll_write_mask(dio, 0)
#define SWD_CLOCK_IDLE() dedic_gpio_cpu_ll_write_mask(clk, s_clk_idle_low ? 0 : clk)
#include "probe_swd_transfer.inc"
#undef SWD_WRITE_BIT
#undef SWD_READ_BIT
#undef SWD_CLOCK_CYCLE
#undef SWD_DIO_HIGH
#undef SWD_DIO_LOW
#undef SWD_CLOCK_IDLE
}

uint8_t probe_swd_transfer(uint8_t request, uint32_t *data)
{
    if (s_spi_selected) {
        if (s_clock_hz < SWD_SPI_MIN_HZ) {
            return transfer_regular(request, data);
        }
        uint8_t ack = swd_spi_transfer(request, data, s_turn_cycles, s_idle_cycles,
                                       s_clk_idle_low != 0);
        if (s_timing_enabled) {
            ++s_timing.acks[ack & 7u];
            swd_spi_stats_t spi;
            swd_spi_get_stats(&spi);
            s_timing.parity_errors = spi.parity_errors;
        }
        return ack;
    }
    if (s_half_nops == 0 && s_fast_enabled && s_fast_out && esp_cpu_get_core_id() == 1) {
        if (!s_fast_active) {
            uint32_t out = REG_READ(GPIO_OUT_REG);
            uint32_t value = ((out & s_clk_mask) ? s_fast_clk_mask : 0) |
                             ((out & s_dio_mask) ? s_fast_dio_mask : 0);
            dedic_gpio_cpu_ll_write_mask(s_fast_clk_mask | s_fast_dio_mask, value);
            esp_rom_gpio_connect_out_signal(s_clk, s_fast_clk_signal, false, false);
            esp_rom_gpio_connect_out_signal(s_dio, s_fast_dio_signal, false, false);
            __asm__ volatile("memw" ::: "memory");
            s_fast_active = true;
        }
        return transfer_dedicated(request, data);
    }
    return transfer_regular(request, data);
}

void probe_swd_output_bits(uint32_t count, const uint8_t *data)
{
    dio_out();
    for (uint32_t i = 0; i < count; ++i) {
        wr_bit((data[i >> 3] >> (i & 7)) & 1);
    }
    clk_idle();
}

void probe_swd_input_bits(uint32_t count, uint8_t *data)
{
    memset(data, 0, (count + 7) / 8);
    dio_in();
    for (uint32_t i = 0; i < count; ++i) {
        if (rd_bit()) {
            data[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }
    dio_out();
    dio_hi();
}

void probe_swd_line_reset(void)
{
    dio_out();
    for (int i = 0; i < 56; ++i) {      /* >= 50 个高电平 */
        wr_bit(1);
    }
    /* 这里不能附带低电平 idle：JTAG→SWD 时，第一次线复位后必须紧接着
     * 发送 0xE79E；若插入 TMS=0 时钟，JTAG TAP 会离开 Test-Logic-Reset。
     * SWD 所需的 idle 由 probe_connect() 在第二次线复位后显式输出。 */
    dio_hi();
    clk_idle();
}

void probe_swd_idle(uint32_t cycles)
{
    dio_lo();
    dio_out();
    while (cycles--) {
        clk_cycle();
    }
    dio_hi();
    clk_idle();
}

void probe_swd_set_reset(bool asserted)
{
    if (s_nrst < 0) {
        return;
    }
    gpio_set_level(s_nrst, asserted ? 0 : 1);
    esp_rom_delay_us(10);
}

bool probe_swd_get_reset(void)
{
    if (s_nrst < 0) {
        return false;
    }
    return gpio_get_level(s_nrst) == 0;
}

void probe_swd_pins_write(uint8_t mask, uint8_t value)
{
    if (mask & SWD_PIN_SWCLK) {
        if (value & SWD_PIN_SWCLK) {
            clk_hi();
        } else {
            clk_lo();
        }
    }
    if (mask & SWD_PIN_SWDIO) {
        dio_out();
        if (value & SWD_PIN_SWDIO) {
            dio_hi();
        } else {
            dio_lo();
        }
    }
    if (mask & SWD_PIN_NRESET) {
        probe_swd_set_reset((value & SWD_PIN_NRESET) == 0);
    }
}

uint8_t probe_swd_pins_read(void)
{
    uint32_t in = REG_READ(GPIO_IN_REG);
    uint8_t v = 0;
    if (in & s_clk_mask) {
        v |= SWD_PIN_SWCLK;
    }
    if (in & s_dio_mask) {
        v |= SWD_PIN_SWDIO;
    }
    if (s_nrst >= 0 && gpio_get_level(s_nrst)) {
        v |= SWD_PIN_NRESET;
    }
    return v;
}

void probe_swd_delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

/* ---------------- 诊断（不影响正常时序） ---------------- */

void probe_swd_diag(int *dio_up, int *dio_down, int *clk_up, int *clk_down, int *nrst)
{
    probe_swd_leave_fast();
    gpio_config_t in_up = {
        .pin_bit_mask = (1ULL << s_dio) | (1ULL << s_clk),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t in_down = {
        .pin_bit_mask = (1ULL << s_dio) | (1ULL << s_clk),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&in_up);
    esp_rom_delay_us(2000);
    if (dio_up) {
        *dio_up = gpio_get_level(s_dio);
    }
    if (clk_up) {
        *clk_up = gpio_get_level(s_clk);
    }

    gpio_config(&in_down);
    esp_rom_delay_us(2000);
    if (dio_down) {
        *dio_down = gpio_get_level(s_dio);
    }
    if (clk_down) {
        *clk_down = gpio_get_level(s_clk);
    }

    gpio_config_t clk_io = {
        .pin_bit_mask = (1ULL << s_clk),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t dio_io = {
        .pin_bit_mask = (1ULL << s_dio),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&clk_io);
    gpio_config(&dio_io);
    clk_hi();
    dio_hi();
    dio_out();

    if (nrst) {
        *nrst = (s_nrst >= 0) ? gpio_get_level(s_nrst) : -1;
    }
}

int probe_swd_release_test(void)
{
    probe_swd_leave_fast();
    dio_hi();
    dio_out();
    esp_rom_delay_us(100);
    int driven = (REG_READ(GPIO_IN_REG) & s_dio_mask) ? 1 : 0;

    dio_in();
    esp_rom_delay_us(100);
    gpio_pullup_dis(s_dio);
    gpio_pulldown_en(s_dio);
    esp_rom_delay_us(2000);
    int released = (REG_READ(GPIO_IN_REG) & s_dio_mask) ? 1 : 0;
    gpio_pulldown_dis(s_dio);
    gpio_pullup_en(s_dio);

    clk_hi();
    esp_rom_delay_us(100);
    int clk_hi_rb = (REG_READ(GPIO_IN_REG) & s_clk_mask) ? 1 : 0;
    clk_lo();
    esp_rom_delay_us(100);
    int clk_lo_rb = (REG_READ(GPIO_IN_REG) & s_clk_mask) ? 1 : 0;

    dio_hi();
    clk_hi();
    dio_out();

    return (driven & 1) | ((released & 1) << 1) | ((clk_hi_rb & 1) << 2) | ((clk_lo_rb & 1) << 3);
}

uint32_t probe_swd_nrst_rise_us(void)
{
    if (s_nrst < 0) {
        return 0xFFFFFFFFu;
    }
    gpio_set_level(s_nrst, 0);
    esp_rom_delay_us(5000);
    uint32_t t0 = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFu);
    gpio_set_level(s_nrst, 1);
    for (int i = 0; i < 20000; ++i) {
        if (gpio_get_level(s_nrst)) {
            uint32_t t1 = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFu);
            return t1 - t0;
        }
    }
    return 0xFFFFFFFEu;
}

/* ---------------- ADC 电压（仅粗测，精度有限） ---------------- */

static adc_oneshot_unit_handle_t s_adc_unit;

static adc_channel_t gpio_to_adc1_ch(int gpio)
{
    if (gpio >= 1 && gpio <= 10) {
        return (adc_channel_t)(gpio - 1);
    }
    return (adc_channel_t)-1;
}

static int adc_mv(int gpio, bool pull_down)
{
    if (gpio < 1 || gpio > 10) {
        return -1;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = pull_down ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return -1;
    }
    if (s_adc_unit == NULL) {
        adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
        if (adc_oneshot_new_unit(&u, &s_adc_unit) != ESP_OK) {
            return -1;
        }
    }
    adc_channel_t ch = gpio_to_adc1_ch(gpio);
    adc_oneshot_chan_cfg_t cc = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc_unit, ch, &cc) != ESP_OK) {
        return -1;
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc_unit, ch, &raw) != ESP_OK) {
        return -1;
    }
    return raw * 3100 / 4095;
}

void probe_swd_diag_mv(int *dio_mv_down, int *clk_mv_up, int *nrst_mv)
{
    probe_swd_leave_fast();
    if (dio_mv_down) {
        *dio_mv_down = adc_mv(s_dio, true);
    }
    if (clk_mv_up) {
        *clk_mv_up = adc_mv(s_clk, false);
    }
    if (nrst_mv) {
        *nrst_mv = (s_nrst >= 0) ? adc_mv(s_nrst, false) : -1;
    }

    gpio_config_t clk_io = {
        .pin_bit_mask = (1ULL << s_clk),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t dio_io = {
        .pin_bit_mask = (1ULL << s_dio),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&clk_io);
    gpio_config(&dio_io);
    if (s_nrst >= 0) {
        gpio_config_t rst = {
            .pin_bit_mask = (1ULL << s_nrst),
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst);
        gpio_set_level(s_nrst, 1);
    }
    clk_hi();
    dio_hi();
    dio_out();
}
