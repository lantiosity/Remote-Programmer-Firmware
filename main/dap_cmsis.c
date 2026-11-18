/**
 * @file  dap_cmsis.c
 * @brief  CMSIS-DAP v1 命令层（USB HID，64 字节报文）
 *
 * 支持的命令：
 *   DAP_Info / HostStatus / Connect / Disconnect / WriteABORT / Delay /
 *   ResetTarget / SWJ_Pins / SWJ_Clock / SWJ_Sequence / SWD_Configure /
 *   TransferConfigure / Transfer / TransferBlock / TransferAbort / ExecuteCommands
 * 只实现 SWD（JTAG 返回失败），与 Keil MDK、pyOCD、OpenOCD 的 cmsis-dap 驱动兼容。
 */
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_app_desc.h"

#include "app_cfg.h"
#include "dap_cmsis.h"
#include "probe_core.h"
#include "wired_ota.h"

static const char *TAG = "dap";

/* 命令码（CMSIS-DAP v2 规范） */
#define CMD_INFO                0x00
#define CMD_HOST_STATUS         0x01
#define CMD_CONNECT             0x02
#define CMD_DISCONNECT          0x03
#define CMD_TRANSFER_CONFIGURE  0x04
#define CMD_TRANSFER            0x05
#define CMD_TRANSFER_BLOCK      0x06
#define CMD_TRANSFER_ABORT      0x07
#define CMD_WRITE_ABORT         0x08
#define CMD_DELAY               0x09
#define CMD_RESET_TARGET        0x0A
#define CMD_SWJ_PINS            0x10
#define CMD_SWJ_CLOCK           0x11
#define CMD_SWJ_SEQUENCE        0x12
#define CMD_SWD_CONFIGURE       0x13
#define CMD_JTAG_SEQUENCE       0x14
#define CMD_JTAG_CONFIGURE      0x15
#define CMD_JTAG_IDCODE         0x16
#define CMD_SWO_TRANSPORT       0x17
#define CMD_SWO_MODE            0x18
#define CMD_SWO_BAUDRATE        0x19
#define CMD_SWO_CONTROL         0x1A
#define CMD_SWO_STATUS          0x1B
#define CMD_SWO_EXT_STATUS      0x1C
#define CMD_SWO_DATA            0x1D
#define CMD_EXECUTE_COMMANDS    0x7F

/* DAP_Info 编号（CMSIS-DAP v2 规范） */
#define INFO_VENDOR             0x01
#define INFO_PRODUCT            0x02
#define INFO_SERIAL             0x03
#define INFO_FW_VERSION         0x04
#define INFO_TARGET_VENDOR      0x05
#define INFO_TARGET_NAME        0x06
#define INFO_BOARD_VENDOR       0x07
#define INFO_BOARD_NAME         0x08
#define INFO_PRODUCT_FW_VER     0x09
#define INFO_CAPABILITIES       0xF0
#define INFO_TEST_DOMAIN_TIMER  0xF1
#define INFO_SWO_BUFFER_SIZE    0xFD
#define INFO_PACKET_COUNT       0xFE
#define INFO_PACKET_SIZE        0xFF

/* 能力位 */
#define DAP_CAP_SWD             (1u << 0)
#define DAP_CAP_SWD_SEQUENCE    (1u << 6)

#define DAP_OK                  0x00
#define DAP_ERROR               0xFF

static uint32_t s_match_mask = 0;
static uint16_t s_match_retry = 0;

void dap_cmsis_reset_session(void)
{
    s_match_mask = 0;
    s_match_retry = 0;
}

/* DAP_Transfer 的请求字不是线上的 8 位 SWD 请求帧。CMSIS-DAP 使用
 * bit0=APnDP、bit1=RnW、bit[3:2]=A[3:2]，其余为 match/timestamp 标志。 */
#define CMSIS_REQ_AP          0x01u
#define CMSIS_REQ_READ        0x02u
#define CMSIS_REQ_MATCH_VALUE 0x10u
#define CMSIS_REQ_MATCH_MASK  0x20u
#define CMSIS_ACK_MISMATCH    0x10u

static uint8_t cmsis_to_swd_request(uint8_t request)
{
    uint8_t swd = 0x81u;                    /* START=1、STOP=0、PARK=1 */
    /* 线上的 START 占据 bit0，因此 APnDP/RnW/A[3:2] 整组从
     * CMSIS-DAP 的 bit[3:0] 左移到 SWD 帧的 bit[4:1]。 */
    swd |= (uint8_t)((request & 0x0Fu) << 1);
    if (__builtin_popcount(swd & 0x1Eu) & 1) {
        swd |= 0x20u;                       /* 对 APnDP/RnW/A[3:2] 做奇校验 */
    }
    return swd;
}

/* 每个 CMSIS-DAP AP 读必须在返回前补 DP.RDBUFF，把 posted-read 的
 * 真正结果取回。源请求只含低 4 位地址，不能原样发给物理层。 */
static uint8_t cmsis_transfer(uint8_t request, uint32_t *data)
{
    uint8_t ack = probe_transfer(cmsis_to_swd_request(request), data);
    if (ack == SWD_ACK_OK &&
        (request & (CMSIS_REQ_AP | CMSIS_REQ_READ)) == (CMSIS_REQ_AP | CMSIS_REQ_READ)) {
        ack = probe_transfer(cmsis_to_swd_request(CMSIS_REQ_READ | 0x0Cu), data);
    }
    return ack;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t add_info_string(uint8_t *resp, uint32_t resp_size, const char *s)
{
    size_t len = strlen(s);
    /* CMSIS-DAP: 字符串响应包含结尾 '\0'，长度也把 '\0' 算进去 */
    if (len + 3 > resp_size) {
        len = resp_size - 3;
    }
    resp[1] = (uint8_t)(len + 1);
    memcpy(&resp[2], s, len);
    resp[2 + len] = 0;
    return (uint32_t)(len + 3);
}

uint32_t dap_cmsis_process(const uint8_t *req, uint32_t req_len,
                           uint8_t *resp, uint32_t resp_size, dap_client_t *client)
{
    if (req_len == 0 || resp_size < 2) {
        return 0;
    }

    const uint8_t cmd = req[0];
    resp[0] = cmd;

    if (cmd == CMD_CONNECT && req_len > 1 && req[1] > 1) {
        resp[1] = 0;
        return 2;
    }
    if (cmd != CMD_INFO && cmd != CMD_HOST_STATUS && cmd != 0x80 &&
        !dap_session_access(client, cmd == CMD_CONNECT ? DAP_OPEN :
                           cmd == CMD_DISCONNECT ? DAP_CLOSE : DAP_USE)) {
        resp[1] = cmd == CMD_CONNECT ? 0 : DAP_ERROR;
        if (cmd == CMD_TRANSFER) {
            resp[1] = 0;
            resp[2] = SWD_ACK_ERROR;
            return 3;
        }
        if (cmd == CMD_TRANSFER_BLOCK) {
            resp[1] = resp[2] = 0;
            resp[3] = SWD_ACK_ERROR;
            return 4;
        }
        if (cmd == CMD_RESET_TARGET) {
            resp[2] = 0;
            return 3;
        }
        return 2;
    }

    switch (cmd) {
    case 0x80: /* CMSIS-DAP vendor self-OTA extension */
        return wired_ota_usb(req, req_len, resp, resp_size);
    case CMD_INFO: {
        uint8_t id = (req_len > 1) ? req[1] : 0;
        if (id == 0) {
            resp[1] = 0;    /* 列出所有可用 ID */
            return 2;
        }
        switch (id) {
        case INFO_VENDOR:
            return add_info_string(resp, resp_size, "ESP-SWD");
        case INFO_PRODUCT:
            return add_info_string(resp, resp_size, "ESP32-S3 DAPLink");
        case INFO_SERIAL:
            return add_info_string(resp, resp_size, cfg_serial_number());
        case INFO_FW_VERSION:
            return add_info_string(resp, resp_size, esp_app_get_description()->version);
        case INFO_TARGET_VENDOR:
            return add_info_string(resp, resp_size, "Generic");
        case INFO_TARGET_NAME:
            return add_info_string(resp, resp_size, "Cortex-M");
        case INFO_BOARD_VENDOR:
            return add_info_string(resp, resp_size, "ESP-SWD");
        case INFO_BOARD_NAME:
            return add_info_string(resp, resp_size, "ESP32-S3 DevKit");
        case INFO_PRODUCT_FW_VER:
            return add_info_string(resp, resp_size, esp_app_get_description()->version);
        case INFO_PACKET_COUNT: {
            resp[1] = 1;
            resp[2] = 1;    /* 一次可处理 1 个数据包 */
            return 3;
        }
        case INFO_PACKET_SIZE: {
            resp[1] = 2;
            resp[2] = (uint8_t)(DAP_PACKET_SIZE & 0xFF);
            resp[3] = (uint8_t)(DAP_PACKET_SIZE >> 8);
            return 4;
        }
        case INFO_CAPABILITIES: {
            uint32_t caps = DAP_CAP_SWD;   /* 只报 SWD；bit6 是 Atomic Commands，未实现 */
            resp[1] = 1;
            resp[2] = (uint8_t)caps;
            return 3;
        }
        default:
            resp[1] = 0;
            return 2;
        }
    }

    case CMD_HOST_STATUS:
        resp[1] = DAP_OK;
        return 2;

    case CMD_CONNECT:
        (void)probe_connect();
        resp[1] = 1;
        return 2;

    case CMD_DISCONNECT:
        resp[1] = DAP_OK;
        return 2;

    case CMD_WRITE_ABORT: {
        uint32_t value = (req_len >= 6) ? rd_u32(&req[2]) : 0;
        /* ABORT 寄存器 = DP 地址 0x00 的写操作：start/APnDP=0/RnW=0/A=0/parity=0/stop/park */
        uint8_t request = 0x81;
        (void)probe_transfer(request, &value);
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_DELAY: {
        uint32_t us = (req_len >= 3) ? (uint32_t)(req[1] | (req[2] << 8)) : 0;
        probe_delay_us(us);
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_RESET_TARGET: {
        probe_status_t st = probe_reset_target();
        resp[1] = (st == PROBE_OK) ? DAP_OK : DAP_ERROR;
        resp[2] = 0; /* No device-specific reset sequence was executed. */
        return 3;
    }

    case CMD_SWJ_PINS: {
        if (req_len >= 7) {
            uint8_t out = req[1];
            uint8_t sel = req[2];
            uint32_t wait_us = rd_u32(&req[3]);
            probe_pins_write(sel, out);
            if (wait_us) {
                probe_delay_us(wait_us);
            }
        }
        resp[1] = probe_pins_read();
        return 2;
    }

    case CMD_SWJ_CLOCK: {
        uint32_t hz = (req_len >= 5) ? rd_u32(&req[1]) : 0;
        if (hz) {
            probe_set_clock(hz);
        }
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_SWJ_SEQUENCE: {
        uint32_t count = (req_len > 1) ? req[1] : 0;
        if (count == 0) {
            count = 256;
        }
        uint32_t bytes = (count + 7) / 8;
        if (req_len >= 2 + bytes) {
            probe_swj_sequence(count, &req[2]);
        }
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_SWD_CONFIGURE: {
        /* bit[1:0] turnaround = 配置值 + 1；bit2 data_phase（不支持） */
        if (req_len >= 2) {
            probe_set_turnaround((uint32_t)((req[1] & 0x03u) + 1u));
        }
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_JTAG_CONFIGURE:
    case CMD_JTAG_SEQUENCE:
        resp[1] = DAP_OK;
        return 2;

    case CMD_SWO_TRANSPORT:
    case CMD_SWO_MODE:
    case CMD_SWO_BAUDRATE:
    case CMD_SWO_CONTROL:
        resp[1] = DAP_ERROR;
        return 2;

    case CMD_SWO_STATUS:
        resp[1] = 0;
        resp[2] = 0;
        resp[3] = 0;
        resp[4] = 0;
        resp[5] = 0;
        resp[6] = 0;
        return 7;

    case CMD_SWO_DATA:
        resp[1] = DAP_ERROR;
        return 2;

    case CMD_JTAG_IDCODE: {
        resp[1] = DAP_OK;
        resp[2] = 0;
        resp[3] = 0;
        resp[4] = 0;
        resp[5] = 0;
        return 6;
    }

    case CMD_TRANSFER_CONFIGURE: {
        if (req_len >= 7) {
            uint8_t idle = req[1];
            uint32_t wait_retry = (uint32_t)(req[2] | (req[3] << 8));
            uint32_t match_retry = (uint32_t)(req[4] | (req[5] << 8));
            s_match_retry = (uint16_t)match_retry;
            probe_set_idle_cycles(idle);
            probe_set_wait_retry(wait_retry);
            probe_set_match_retry(match_retry);
        }
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_TRANSFER: {
        const uint32_t count = (req_len > 2) ? req[2] : 0;
        uint32_t pos = 3;
        uint32_t out = 3;          /* cmd、完成数、整包唯一的 ACK */
        uint8_t done = 0;
        uint8_t transfer_ack = SWD_ACK_OK;
        bool need_write_check = false;

        for (uint32_t i = 0; i < count; ++i) {
            if (pos >= req_len) {
                transfer_ack = SWD_ACK_ERROR;
                break;
            }
            const uint8_t request = req[pos++];
            const bool is_read = (request & CMSIS_REQ_READ) != 0;
            const bool match_value = is_read && ((request & CMSIS_REQ_MATCH_VALUE) != 0);
            uint32_t data = 0;
            uint32_t expected = 0;

            /* 普通写、MATCH_MASK 写以及 MATCH_VALUE 读都带一个 32 位参数。 */
            if (!is_read || match_value) {
                if (pos + 4 > req_len) {
                    transfer_ack = SWD_ACK_ERROR;
                    break;
                }
                data = rd_u32(&req[pos]);
                expected = data;
                pos += 4;
            }
            if (!is_read && (request & CMSIS_REQ_MATCH_MASK)) {
                s_match_mask = data;         /* 只设置掩码，不发起 SWD 写事务 */
                transfer_ack = SWD_ACK_OK;
            } else if (is_read) {
                if (!match_value && out + 4 > resp_size) {
                    transfer_ack = SWD_ACK_ERROR;
                    break;
                }
                uint32_t attempts = match_value ? (uint32_t)s_match_retry + 1u : 1u;
                do {
                    transfer_ack = cmsis_transfer(request, &data);
                    if (transfer_ack != SWD_ACK_OK) {
                        break;
                    }
                    if (!match_value || (data & s_match_mask) == expected) {
                        break;
                    }
                } while (--attempts != 0);
                if (transfer_ack == SWD_ACK_OK && match_value &&
                    (data & s_match_mask) != expected) {
                    transfer_ack |= CMSIS_ACK_MISMATCH;
                }
                /* Value-match polls return status only. Returning their value
                 * shifts every later ordinary read in the same HID response. */
                if (transfer_ack == SWD_ACK_OK && !match_value) {
                    memcpy(&resp[out], &data, 4);
                    out += 4;
                }
                need_write_check = false;
            } else {
                transfer_ack = cmsis_transfer(request, &data);
                need_write_check = true;
            }

            if (transfer_ack != SWD_ACK_OK) {
                break;
            }
            ++done;
        }

        /* 写事务的错误可能延迟到下一次 DP/AP 访问才报告；以一个无返回数据的
         * RDBUFF 读收尾，避免把已失败的写误报为成功。 */
        if (transfer_ack == SWD_ACK_OK && need_write_check) {
            uint32_t ignored = 0;
            transfer_ack = probe_transfer(cmsis_to_swd_request(CMSIS_REQ_READ | 0x0Cu), &ignored);
        }
        resp[1] = done;
        resp[2] = transfer_ack;
        return out;
    }

    case CMD_TRANSFER_BLOCK: {
        if (req_len < 5) {
            resp[1] = 0;
            resp[2] = 0;
            resp[3] = DAP_ERROR;
            return 4;
        }
        uint32_t count = (uint32_t)(req[2] | (req[3] << 8));
        const uint8_t request = req[4];
        const bool is_read = (request & CMSIS_REQ_READ) != 0;
        uint32_t max_words = (resp_size - 4) / 4;
        uint32_t words;

        if (is_read) {
            words = (count < max_words) ? count : max_words;
        } else {
            uint32_t avail = (req_len - 5) / 4;
            words = (count < avail) ? count : avail;
        }

        static uint32_t s_block[DAP_PACKET_SIZE / 4];
        if (words > (sizeof(s_block) / sizeof(s_block[0]))) {
            words = sizeof(s_block) / sizeof(s_block[0]);
        }
        memset(s_block, 0, words * sizeof(uint32_t));
        if (!is_read) {
            for (uint32_t i = 0; i < words; ++i) {
                s_block[i] = rd_u32(&req[5 + i * 4]);
            }
        }
        uint32_t done = 0;
        uint8_t ack = probe_transfer_block(cmsis_to_swd_request(request), s_block, words, &done);
        if (ack == SWD_ACK_OK && !is_read) {
            uint32_t ignored = 0;
            ack = probe_transfer(cmsis_to_swd_request(CMSIS_REQ_READ | 0x0Cu), &ignored);
        }

        resp[1] = (uint8_t)(done & 0xFF);
        resp[2] = (uint8_t)((done >> 8) & 0xFF);
        resp[3] = ack;
        uint32_t out = 4;
        if (is_read) {
            for (uint32_t i = 0; i < done; ++i) {
                memcpy(&resp[out], &s_block[i], 4);
                out += 4;
            }
        }
        return out;
    }

    case CMD_TRANSFER_ABORT:
        /* 传输是同步阻塞的，这里只需保证上层能继续 */
        resp[1] = DAP_OK;
        return 2;

    case CMD_EXECUTE_COMMANDS:
        resp[1] = 0;
        resp[2] = DAP_ERROR;
        return 3;

    default:
        ESP_LOGD(TAG, "unsupported command 0x%02x", cmd);
        resp[1] = DAP_ERROR;
        return 2;
    }
}
