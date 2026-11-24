/**
 * @file  swd_emu.c
 * @brief  内置 SWD 目标仿真器（中断驱动，自检用）
 *
 * 用途：不接任何外部芯片，验证本机 SWD 主机时序是否正确。
 * 接线（两根短接线）：
 *      GPIO4 (SWCLK) ──> GPIO7 (仿真器 CLK 输入)
 *      GPIO5 (SWDIO) <──> GPIO8 (仿真器 DIO 双向)
 *
 * 控制台：
 *      swdemu on     启动（同时把 SWD 时钟降到 100kHz）
 *      scan          应读到 DP IDCODE = 0x6BA02477
 *      swdemu dump   查看收到的请求统计
 *      swdemu off    停止
 *
 * 注意：中断在 SWCLK 的每个边沿触发，1MHz 以上中断开销过大，所以限制在 ≤100kHz。
 */
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#include "probe_core.h"
#include "swd_emu.h"

static const char *TAG = "swdemu";

#define EMU_CLK_GPIO 7
#define EMU_DIO_GPIO 8
#define EMU_CLK_MASK (1u << EMU_CLK_GPIO)
#define EMU_DIO_MASK (1u << EMU_DIO_GPIO)

#define EMU_IDCODE 0x6BA02477u

/* 状态机 */
enum {
    ST_IDLE = 0,   /* 收请求 */
    ST_ACK,        /* 输出 ACK */
    ST_READ,       /* 输出读数据 */
    ST_WRITE,      /* 接收写数据 */
};

static volatile int      s_state;
static volatile uint8_t  s_req;
static volatile int      s_bitcnt;
static volatile int      s_ack;
static volatile int      s_ack_idx;
static volatile uint32_t s_data;
static volatile int      s_data_idx;
static volatile bool     s_is_read;
static volatile bool     s_is_ap;
static volatile uint32_t s_addr;

static volatile bool     s_running;
static volatile uint32_t s_req_count;
static volatile uint32_t s_ok_count;
static volatile uint32_t s_read_count;
static volatile uint8_t  s_last_req;
static volatile uint8_t  s_hist[16];
static volatile uint32_t s_hist_count;

static inline int  dio_in(void) { return (REG_READ(GPIO_IN_REG) & EMU_DIO_MASK) ? 1 : 0; }
static inline void dio_set(int v)
{
    if (v) {
        REG_WRITE(GPIO_OUT_W1TS_REG, EMU_DIO_MASK);
    } else {
        REG_WRITE(GPIO_OUT_W1TC_REG, EMU_DIO_MASK);
    }
}
static inline void dio_out(void) { REG_WRITE(GPIO_ENABLE_W1TS_REG, EMU_DIO_MASK); }
static inline void dio_zz(void) { REG_WRITE(GPIO_ENABLE_W1TC_REG, EMU_DIO_MASK); }

static bool IRAM_ATTR req_valid(uint8_t r)
{
    if ((r & 0x81u) != 0x81u || (r & 0x40u) != 0) {
        return false;   /* start/park 必须为 1，stop 必须为 0 */
    }
    const int par = __builtin_popcount(r & 0x1Eu) & 1;
    return par == ((r >> 5) & 1);
}

static void IRAM_ATTR emu_isr(void *arg)
{
    (void)arg;
    const bool rising = (REG_READ(GPIO_IN_REG) & EMU_CLK_MASK) != 0;

    if (rising) {
        switch (s_state) {
        case ST_IDLE:
            if (dio_in()) {
                s_req |= (uint8_t)(1u << s_bitcnt);
            }
            if (++s_bitcnt >= 8) {
                s_bitcnt = 0;
                s_req_count++;
                s_last_req = s_req;
                s_hist[s_hist_count & 15] = s_req;
                s_hist_count++;
                if (req_valid(s_req)) {
                    s_ok_count++;
                    s_is_read = (s_req & 0x04u) != 0;
                    s_is_ap = (s_req & 0x02u) != 0;
                    s_addr = ((uint32_t)(s_req >> 3) & 0x03u) << 2;
                    s_ack = 1;          /* ACK = 0b001 (OK) */
                    s_ack_idx = 1;
                    s_state = ST_ACK;
                    dio_set(1);
                    dio_out();          /* ACK bit0 */
                }
                s_req = 0;
            }
            break;

        case ST_WRITE:
            if (s_data_idx < 32 && dio_in()) {
                s_data |= (1u << s_data_idx);
            }
            if (++s_data_idx >= 33) {
                s_state = ST_IDLE;
                s_data_idx = 0;
            }
            break;

        default:
            break;
        }
        return;
    }

    /* 下降沿 */
    switch (s_state) {
    case ST_ACK:
        if (s_ack_idx < 3) {
            dio_set((s_ack >> s_ack_idx) & 1);
            s_ack_idx++;
        } else if (s_is_read) {
            s_data = (!s_is_ap && s_addr == 0x00) ? EMU_IDCODE : 0x00000000u;
            s_state = ST_READ;
            s_data_idx = 0;
            dio_set(s_data & 1u);   /* data bit0 */
            dio_out();
            s_data_idx = 1;
            s_read_count++;
        } else {
            s_state = ST_WRITE;
            s_data_idx = 0;
            s_data = 0;
            dio_zz();               /* 主机驱动数据 */
        }
        break;

    case ST_READ: {
        int bit;
        if (s_data_idx < 32) {
            bit = (int)((s_data >> s_data_idx) & 1u);
        } else {
            bit = __builtin_popcount(s_data) & 1;   /* 校验位 */
        }
        if (s_data_idx <= 32) {
            dio_set(bit);
            s_data_idx++;
        } else {
            dio_zz();
            s_state = ST_IDLE;
            s_data_idx = 0;
        }
        break;
    }

    default:
        break;
    }
}

esp_err_t swd_emu_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    gpio_config_t clk = {
        .pin_bit_mask = (1ULL << EMU_CLK_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config_t dio = {
        .pin_bit_mask = (1ULL << EMU_DIO_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&clk);
    gpio_config(&dio);
    dio_zz();

    s_state = ST_IDLE;
    s_req = 0;
    s_bitcnt = 0;
    s_req_count = 0;
    s_ok_count = 0;
    s_read_count = 0;
    s_hist_count = 0;

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_isr_handler_add(EMU_CLK_GPIO, emu_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add 失败: %s", esp_err_to_name(err));
        return err;
    }
    gpio_intr_enable(EMU_CLK_GPIO);

    s_running = true;
    /* 中断驱动的仿真器只适合低速，顺手把主机时钟降下来 */
    probe_set_clock(100000);
    ESP_LOGI(TAG, "SWD 仿真器已启动（CLK=GPIO%d, DIO=GPIO%d），SWD 时钟已设为 100kHz",
             EMU_CLK_GPIO, EMU_DIO_GPIO);
    return ESP_OK;
}

void swd_emu_stop(void)
{
    if (!s_running) {
        return;
    }
    gpio_intr_disable(EMU_CLK_GPIO);
    gpio_isr_handler_remove(EMU_CLK_GPIO);
    dio_zz();
    s_running = false;
    ESP_LOGI(TAG, "SWD 仿真器已停止");
}

bool swd_emu_is_running(void)
{
    return s_running;
}

void swd_emu_dump(uint8_t *buf, uint32_t buf_len, uint32_t *total,
                  uint32_t *valid, uint32_t *reads)
{
    if (total) {
        *total = s_req_count;
    }
    if (valid) {
        *valid = s_ok_count;
    }
    if (reads) {
        *reads = s_read_count;
    }
    if (buf) {
        uint32_t n = s_hist_count;
        if (n > 16) {
            n = 16;
        }
        if (n > buf_len) {
            n = buf_len;
        }
        for (uint32_t i = 0; i < n; ++i) {
            buf[i] = s_hist[(s_hist_count - n + i) & 15];
        }
    }
}
