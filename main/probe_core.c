/**
 * @file  probe_core.c
 * @brief  DP / AP / MEM-AP 语义层实现。
 *
 * 说明：
 *  - CMSIS-DAP 的 DAP_Transfer 由上位机给出完整请求字（含校验位），这里原样转发；
 *  - pyOCD remote 的 read_ap/write_ap 需要本层自己处理 AP 的 posted read；
 *  - MEM-AP 内存访问（read_mem/write_mem/block）也在这里实现，供 WiFi/BLE 链路使用。
 */
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "probe_core.h"

static const char *TAG = "probe";

/* DP / AP 寄存器地址 */
#define DP_IDCODE      0x00u
#define DP_ABORT       0x00u
#define DP_CTRL_STAT   0x04u
#define DP_SELECT      0x08u
#define DP_RDBUFF      0x0Cu

#define AP_CSW         0x00u
#define AP_TAR         0x04u
#define AP_DRW         0x0Cu

/* CSW 位域 */
#define CSW_SIZE8      0x0u
#define CSW_SIZE16     0x1u
#define CSW_SIZE32     0x2u
#define CSW_SIZE_MASK  0x07u
#define CSW_SADDRINC   0x10u
#define CSW_ADDRINC_MASK 0x30u
#define CSW_DEVICEEN   0x40u
#define CSW_CONFIG_MASK (CSW_SIZE_MASK | CSW_ADDRINC_MASK | CSW_DEVICEEN)

#define MAX_AP          256u

typedef struct {
    probe_core_cfg_t cfg;
    uint32_t wait_retry;
    uint32_t match_retry;
    uint8_t  idle_cycles;
    bool     connected;
    uint32_t idcode;
    uint32_t apsel;
    uint32_t cur_csw;
    bool     csw_valid;
} probe_state_t;

static SemaphoreHandle_t s_bus_lock;

void probe_lock(void)
{
    if (s_bus_lock) {
        (void)xSemaphoreTakeRecursive(s_bus_lock, portMAX_DELAY);
    }
}

void probe_unlock(void)
{
    if (s_bus_lock) {
        (void)xSemaphoreGiveRecursive(s_bus_lock);
    }
}

static probe_state_t s_probe = {
    .wait_retry = 100,
    .match_retry = 0,
    .idle_cycles = 0,
    .apsel = 0xFFFFFFFFu,
};

/* ---------------- 工具函数 ---------------- */

static inline uint8_t make_request(bool ap, bool rnw, uint32_t addr)
{
    uint8_t req = 0x81u;                    /* start=1, park=1 */
    if (ap) {
        req |= 0x02u;
    }
    if (rnw) {
        req |= 0x04u;
    }
    req |= (uint8_t)(((addr >> 2) & 0x03u) << 3);
    if (__builtin_popcount(req & 0x1Eu) & 1) {
        req |= 0x20u;                       /* 奇校验 */
    }
    return req;
}

static probe_status_t ack_to_status(uint8_t ack)
{
    switch (ack) {
    case SWD_ACK_OK:
        return PROBE_OK;
    case SWD_ACK_WAIT:
        return PROBE_ERR_WAIT;
    case SWD_ACK_FAULT:
        return PROBE_ERR_FAULT;
    default:
        return PROBE_ERR_PARITY;
    }
}

static uint32_t size_code(uint32_t size)
{
    switch (size) {
    case 8:
        return CSW_SIZE8;
    case 16:
        return CSW_SIZE16;
    default:
        return CSW_SIZE32;
    }
}

/* ---------------- 初始化 / 连接 ---------------- */

esp_err_t probe_init(const probe_core_cfg_t *cfg)
{
    swd_pins_t pins = {
        .clk_gpio = cfg->clk_gpio,
        .dio_gpio = cfg->dio_gpio,
        .nrst_gpio = cfg->nrst_gpio,
    };
    esp_err_t err = probe_swd_init(&pins);
    if (err != ESP_OK) {
        return err;
    }
    if (s_bus_lock == NULL) {
        s_bus_lock = xSemaphoreCreateRecursiveMutex();
    }
    s_probe.cfg = *cfg;
    probe_swd_set_clock(cfg->clock_hz ? cfg->clock_hz : 1000000);
    s_probe.connected = false;
    s_probe.apsel = 0xFFFFFFFFu;
    s_probe.csw_valid = false;
    return ESP_OK;
}

esp_err_t probe_connect(void)
{
    probe_swd_prepare_fast();
    /* SWJ-DP 上电默认是 JTAG 模式，必须先做：
     *   线复位 -> JTAG-to-SWD 序列(0xE79E) -> 线复位
     * 这样即使上位机没有自己发切换序列，我们也能进 SWD。 */
    probe_swd_line_reset();
    const uint8_t jtag_to_swd[2] = {0x9E, 0xE7};   /* 0xE79E，LSB 先出 */
    probe_swd_output_bits(16, jtag_to_swd);
    probe_swd_line_reset();
    probe_swd_idle(2);

    uint32_t idcode = 0;
    uint8_t ack = probe_transfer(make_request(false, true, DP_IDCODE), &idcode);
    s_probe.idcode = (ack == SWD_ACK_OK) ? idcode : 0;
    if (ack == SWD_ACK_OK) {
        /* 调试域上电：不置 CSYSPWRUPREQ/CDBGPWRUPREQ 的话，MEM-AP 访问一律返回 FAULT */
        (void)probe_dp_write(DP_CTRL_STAT, 0x50000000u);
        uint32_t cs = 0;
        for (int i = 0; i < 100; ++i) {
            if (probe_dp_read(DP_CTRL_STAT, &cs) == PROBE_OK && (cs & 0xA0000000u) == 0xA0000000u) {
                break;
            }
            vTaskDelay(1);
        }
        /* 清粘滞错误：ABORT 是 DP 0x00 的写事务，W1C */
        (void)probe_dp_write(0x00u, 0x1Eu);
        ESP_LOGI(TAG, "调试域已上电 CTRL/STAT=0x%08" PRIx32, cs);
    }
    s_probe.connected = (ack == SWD_ACK_OK);
    s_probe.apsel = 0xFFFFFFFFu;
    s_probe.csw_valid = false;

    if (ack == SWD_ACK_OK) {
        ESP_LOGI(TAG, "SWD connected, IDCODE=0x%08" PRIx32, idcode);
    } else {
        ESP_LOGW(TAG, "SWD IDCODE read failed (ack=0x%02x), 目标可能未上电/未接线", ack);
    }
    return ESP_OK;
}

esp_err_t probe_disconnect(void)
{
    probe_swd_leave_fast();
    s_probe.connected = false;
    s_probe.apsel = 0xFFFFFFFFu;
    s_probe.csw_valid = false;
    return ESP_OK;
}

bool probe_is_connected(void)
{
    return s_probe.connected;
}

uint32_t probe_get_clock(void)
{
    return probe_swd_get_clock();
}

void probe_set_clock(uint32_t hz)
{
    probe_swd_set_clock(hz);
    ESP_LOGI(TAG, "SWD clock -> %" PRIu32 " Hz", probe_swd_get_clock());
}

void probe_set_idle_cycles(uint8_t cycles)
{
    s_probe.idle_cycles = cycles;
    probe_swd_set_idle(cycles);
}

void probe_set_wait_retry(uint32_t retry)
{
    s_probe.wait_retry = retry;
}

void probe_set_match_retry(uint32_t retry)
{
    s_probe.match_retry = retry;
}

uint32_t probe_idcode(void)
{
    return s_probe.idcode;
}

/* ---------------- 原始传输 ---------------- */

uint8_t probe_transfer(uint8_t request, uint32_t *data)
{
    uint32_t dummy = 0;
    if (data == NULL) {
        data = &dummy;
    }
    uint32_t retry = s_probe.wait_retry;
    for (;;) {
        uint8_t ack = probe_swd_transfer(request, data);
        if (ack != SWD_ACK_WAIT) {
            return ack;
        }
        if (retry == 0) {
            return ack;
        }
        --retry;
    }
}

uint8_t probe_transfer_block(uint8_t request, uint32_t *data, uint32_t count, uint32_t *done)
{
    const bool is_read = (request & 0x04u) != 0;
    const bool is_ap = (request & 0x02u) != 0;
    uint32_t i = 0;

    if (done) {
        *done = 0;
    }
    if (count == 0) {
        return SWD_ACK_OK;
    }

    if (!is_read) {
        for (i = 0; i < count; ++i) {
            uint8_t ack = probe_transfer(request, &data[i]);
            if (ack != SWD_ACK_OK) {
                if (done) {
                    *done = i;
                }
                return ack;
            }
        }
        if (done) {
            *done = count;
        }
        return SWD_ACK_OK;
    }

    if (is_ap) {
        /* AP 读是 posted 的：第 n 次读拿到第 n-1 次的数据，最后用 RDBUFF 收尾 */
        uint32_t prev = 0;
        for (i = 0; i < count; ++i) {
            uint32_t v = 0;
            uint8_t ack = probe_transfer(request, &v);
            if (ack != SWD_ACK_OK) {
                if (done) {
                    *done = i;
                }
                return ack;
            }
            if (i > 0) {
                data[i - 1] = v;
            }
            prev = v;
        }
        uint32_t last = 0;
        uint8_t ack = probe_transfer(make_request(false, true, DP_RDBUFF), &last);
        if (ack != SWD_ACK_OK) {
            (void)prev;
            if (done) {
                *done = count;
            }
            return ack;
        }
        data[count - 1] = last;
    } else {
        for (i = 0; i < count; ++i) {
            uint8_t ack = probe_transfer(request, &data[i]);
            if (ack != SWD_ACK_OK) {
                if (done) {
                    *done = i;
                }
                return ack;
            }
        }
    }
    if (done) {
        *done = count;
    }
    return SWD_ACK_OK;
}

/* ---------------- DP / AP ---------------- */

probe_status_t probe_dp_read(uint32_t addr, uint32_t *value)
{
    uint32_t v = 0;
    probe_status_t st = ack_to_status(probe_transfer(make_request(false, true, addr), &v));
    if (st == PROBE_OK && value) {
        *value = v;
    }
    return st;
}

probe_status_t probe_dp_write(uint32_t addr, uint32_t value)
{
    return ack_to_status(probe_transfer(make_request(false, false, addr), &value));
}

probe_status_t probe_ap_read(uint32_t addr, uint32_t *value)
{
    uint32_t tmp = 0;
    probe_status_t st = ack_to_status(probe_transfer(make_request(true, true, addr), &tmp));
    if (st != PROBE_OK) {
        return st;
    }
    /* posted read：读 RDBUFF 才能拿到真正的值 */
    uint32_t v = 0;
    st = ack_to_status(probe_transfer(make_request(false, true, DP_RDBUFF), &v));
    if (st == PROBE_OK && value) {
        *value = v;
    }
    return st;
}

probe_status_t probe_ap_write(uint32_t addr, uint32_t value)
{
    return ack_to_status(probe_transfer(make_request(true, false, addr), &value));
}

probe_status_t probe_ap_read_multiple(uint32_t addr, uint32_t *values, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        probe_status_t st = probe_ap_read(addr, &values[i]);
        if (st != PROBE_OK) {
            return st;
        }
    }
    return PROBE_OK;
}

probe_status_t probe_ap_write_multiple(uint32_t addr, const uint32_t *values, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = values[i];
        probe_status_t st = ack_to_status(probe_transfer(make_request(true, false, addr), &v));
        if (st != PROBE_OK) {
            return st;
        }
    }
    return PROBE_OK;
}

/* ---------------- 序列 / 引脚 ---------------- */

void probe_swj_sequence(uint32_t bit_count, const uint8_t *data)
{
    probe_swd_output_bits(bit_count, data);
    probe_swd_idle(s_probe.idle_cycles);
}

void probe_swj_sequence_u64(uint32_t bit_count, uint64_t value)
{
    uint8_t buf[8] = {0};
    memcpy(buf, &value, sizeof(buf));
    probe_swj_sequence(bit_count, buf);
}

void probe_pins_write(uint8_t mask, uint8_t value)
{
    probe_swd_pins_write(mask, value);
}

uint8_t probe_pins_read(void)
{
    return probe_swd_pins_read();
}

void probe_delay_us(uint32_t us)
{
    probe_swd_delay_us(us);
}

/* ---------------- 复位 ---------------- */

void probe_assert_reset(bool asserted)
{
    probe_swd_set_reset(asserted);
}

bool probe_is_reset_asserted(void)
{
    return probe_swd_get_reset();
}

probe_status_t probe_reset_target(void)
{
    /* The probe reset command does not identify a Cortex-M core or its MEM-AP.
     * A debugger that selected the core can issue SYSRESETREQ through that AP. */
    if (s_probe.cfg.nrst_gpio < 0) return PROBE_ERR_IO;
    probe_swd_set_reset(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    probe_swd_set_reset(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    probe_swd_line_reset();
    probe_swd_idle(2);
    s_probe.apsel = 0xFFFFFFFFu;
    s_probe.csw_valid = false;
    return PROBE_OK;
}

/* ---------------- MEM-AP ---------------- */

probe_status_t probe_mem_select_ap(uint32_t apsel, uint32_t bank)
{
    if (apsel >= MAX_AP) {
        return PROBE_ERR_ARG;
    }
    /* SELECT: [31:24] APSEL, [7:4] APBANKSEL（不能丢，AP IDR 在 0xFC 即 bank 0xF） */
    uint32_t sel = (apsel << 24) | (bank & 0xF0u);
    if (s_probe.apsel == sel) {
        return PROBE_OK;
    }
    probe_status_t st = probe_dp_write(DP_SELECT, sel);
    if (st != PROBE_OK) {
        return st;
    }
    s_probe.apsel = sel;
    s_probe.csw_valid = false;
    return PROBE_OK;
}

static probe_status_t mem_setup(uint32_t addr, uint32_t size, bool block)
{
    const uint32_t low_cfg = size_code(size) | CSW_DEVICEEN | CSW_SADDRINC;
    probe_status_t st;

    /* CSW 高位包含 HPROT/HNONSEC 等与目标相关的总线属性，不能整体覆盖。
     * 上位机可能已经设置好这些位；每次选中新的 AP/bank 后先读当前值，
     * 仅替换本层负责的大小、地址自增和设备使能字段。 */
    if (!s_probe.csw_valid || (s_probe.cur_csw & CSW_CONFIG_MASK) != low_cfg) {
        uint32_t csw = 0;
        st = probe_ap_read(AP_CSW, &csw);
        if (st != PROBE_OK) {
            return st;
        }
        const uint32_t new_csw = (csw & ~CSW_CONFIG_MASK) | low_cfg;
        if (new_csw != csw) {
            st = probe_ap_write(AP_CSW, new_csw);
            if (st != PROBE_OK) {
                return st;
            }
        }
        s_probe.cur_csw = new_csw;
        s_probe.csw_valid = true;
    }
    (void)block;
    return probe_ap_write(AP_TAR, addr);
}

probe_status_t probe_mem_read(uint32_t addr, uint32_t size, uint32_t *value)
{
    if (size != 8 && size != 16 && size != 32) {
        return PROBE_ERR_ARG;
    }
    probe_status_t st = mem_setup(addr, size, false);
    if (st != PROBE_OK) {
        return st;
    }
    uint32_t v = 0;
    st = probe_ap_read(AP_DRW, &v);
    if (st != PROBE_OK) {
        return st;
    }
    if (size == 8) {
        v = (v >> ((addr & 0x03u) << 3)) & 0xFFu;
    } else if (size == 16) {
        v = (v >> ((addr & 0x02u) << 3)) & 0xFFFFu;
    }
    if (value) {
        *value = v;
    }
    return PROBE_OK;
}

probe_status_t probe_mem_write(uint32_t addr, uint32_t size, uint32_t value)
{
    if (size != 8 && size != 16 && size != 32) {
        return PROBE_ERR_ARG;
    }
    probe_status_t st = mem_setup(addr, size, false);
    if (st != PROBE_OK) {
        return st;
    }
    uint32_t data = value;
    if (size == 8) {
        data = (value & 0xFFu) << ((addr & 0x03u) << 3);
    } else if (size == 16) {
        data = (value & 0xFFFFu) << ((addr & 0x02u) << 3);
    }
    return probe_ap_write(AP_DRW, data);
}

/* ADIv5 guarantees at least 10 TAR auto-increment bits. Reprogram TAR at
 * every 1 KiB boundary; larger target-specific windows are not assumed. */
probe_status_t probe_mem_read_block32(uint32_t addr, uint32_t *data, uint32_t words)
{
    if ((addr & 3u) || (words && !data) || (uint64_t)addr + (uint64_t)words * 4 > 0x100000000ULL)
        return PROBE_ERR_ARG;
    while (words) {
        uint32_t count = (1024u - (addr & 1023u)) / 4;
        if (count > words) count = words;
        probe_status_t st = mem_setup(addr, 32, true);
        if (st != PROBE_OK) return st;
        uint32_t done = 0;
        uint8_t ack = probe_transfer_block(make_request(true, true, AP_DRW), data, count, &done);
        if (ack != SWD_ACK_OK) return ack_to_status(ack);
        if (done != count) return PROBE_ERR_FAULT;
        data += count;
        addr += count * 4;
        words -= count;
    }
    return PROBE_OK;
}

probe_status_t probe_mem_write_block32(uint32_t addr, const uint32_t *data, uint32_t words)
{
    if ((addr & 3u) || (words && !data) || (uint64_t)addr + (uint64_t)words * 4 > 0x100000000ULL)
        return PROBE_ERR_ARG;
    while (words) {
        uint32_t count = (1024u - (addr & 1023u)) / 4;
        if (count > words) count = words;
        probe_status_t st = mem_setup(addr, 32, true);
        if (st != PROBE_OK) return st;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t value = data[i];
            uint8_t ack = probe_transfer(make_request(true, false, AP_DRW), &value);
            if (ack != SWD_ACK_OK) return ack_to_status(ack);
        }
        st = probe_mem_flush();
        if (st != PROBE_OK) return st;
        data += count;
        addr += count * 4;
        words -= count;
    }
    return PROBE_OK;
}

probe_status_t probe_mem_read_block8(uint32_t addr, uint8_t *data, uint32_t bytes)
{
    if ((bytes && !data) || (uint64_t)addr + bytes > 0x100000000ULL) return PROBE_ERR_ARG;
    uint32_t words[64];
    while (bytes) {
        probe_status_t st;
        if ((addr & 3u) || bytes < 4) {
            uint32_t value = 0;
            st = probe_mem_read(addr, 8, &value);
            if (st != PROBE_OK) return st;
            *data++ = (uint8_t)value;
            ++addr; --bytes;
            continue;
        }
        uint32_t count = bytes / 4;
        if (count > 64) count = 64;
        st = probe_mem_read_block32(addr, words, count);
        if (st != PROBE_OK) return st;
        for (uint32_t i = 0; i < count * 4; ++i)
            data[i] = (uint8_t)(words[i / 4] >> ((i & 3u) * 8));
        data += count * 4; addr += count * 4; bytes -= count * 4;
    }
    return PROBE_OK;
}

probe_status_t probe_mem_write_block8(uint32_t addr, const uint8_t *data, uint32_t bytes)
{
    if ((bytes && !data) || (uint64_t)addr + bytes > 0x100000000ULL) return PROBE_ERR_ARG;
    uint32_t words[64];
    while (bytes) {
        probe_status_t st;
        if ((addr & 3u) || bytes < 4) {
            st = probe_mem_write(addr, 8, *data);
            if (st != PROBE_OK) return st;
            ++data; ++addr; --bytes;
            continue;
        }
        uint32_t count = bytes / 4;
        if (count > 64) count = 64;
        for (uint32_t i = 0; i < count; ++i)
            words[i] = (uint32_t)data[i*4] | (uint32_t)data[i*4+1] << 8 |
                       (uint32_t)data[i*4+2] << 16 | (uint32_t)data[i*4+3] << 24;
        st = probe_mem_write_block32(addr, words, count);
        if (st != PROBE_OK) return st;
        data += count * 4; addr += count * 4; bytes -= count * 4;
    }
    return probe_mem_flush();
}

probe_status_t probe_mem_flush(void)
{
    uint32_t v = 0;
    return ack_to_status(probe_transfer(make_request(false, true, DP_RDBUFF), &v));
}

void probe_diag(int *dio_up, int *dio_down, int *clk_up, int *clk_down, int *nrst)
{
    probe_swd_diag(dio_up, dio_down, clk_up, clk_down, nrst);
}

void probe_diag_mv(int *dio_mv_down, int *clk_mv_up, int *nrst_mv)
{
    probe_swd_diag_mv(dio_mv_down, clk_mv_up, nrst_mv);
}

void probe_swap_pins(void)
{
    probe_swd_swap_pins();
    s_probe.apsel = 0xFFFFFFFFu;
    s_probe.csw_valid = false;
}

void probe_set_turnaround(uint32_t cycles)
{
    probe_swd_set_turnaround(cycles);
}
