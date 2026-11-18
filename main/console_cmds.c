/**
 * @file  console_cmds.c
 * @brief  UART0 控制台命令（可与日志共存）
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "soc/gpio_reg.h"

#include "app_cfg.h"
#include "ble_dev.h"
#include "ble_ota.h"
#include "wifi_setup.h"
#include "console_cmds.h"
#include "link.h"
#include "net_dev.h"
#include "probe_core.h"
#include "probe_swd_spi.h"
#include "serial_bridge.h"
#include "swd_emu.h"
#include "usb_dev.h"

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    link_stats_t st;
    link_get_stats(&st);

    printf("\n=== ESP-SWD DAPLink ===\n");
    printf("序列号     : %s\n", cfg_serial_number());
    printf("USB        : %s\n", usb_dev_hid_ready() ? "已连接 (CMSIS-DAP HID + CDC)" : "未连接");
    printf("WiFi STA   : %s  %s\n", net_dev_sta_connected() ? net_dev_ip() : "未连接",
           net_dev_sta_connected() ? net_dev_ssid() : "");
    printf("WiFi 热点  : %s\n", net_dev_ap_ip());
    printf("BLE        : %s\n", ble_dev_ready() ? "已启动" : "关闭");
    printf("SWD 时钟   : %" PRIu32 " kHz\n", probe_get_clock() / 1000u);
    printf("DP IDCODE  : 0x%08" PRIx32 "\n", probe_idcode());
    printf("目标串口   : %d baud\n", serial_bridge_get_baud());
    printf("串口丢弃字节: USB=%" PRIu32 " TCP=%" PRIu32 " BLE=%" PRIu32 "\n",
           serial_bridge_dropped_bytes(SERIAL_OUTPUT_USB),
           serial_bridge_dropped_bytes(SERIAL_OUTPUT_TCP),
           serial_bridge_dropped_bytes(SERIAL_OUTPUT_BLE));
    printf("请求统计   : USB=%" PRIu32 " TCP=%" PRIu32 " BLE=%" PRIu32
           " 丢弃=%" PRIu32 " 最大长度=%" PRIu32 "\n",
           st.usb_req, st.tcp_req, st.ble_req, st.dropped, st.max_req_len);
    printf("pyOCD(WiFi): pyocd flash --probe remote:%s:%d -t <target> <file>\n\n",
           net_dev_ip(), cfg_dap_tcp_port());
    return 0;
}

static int cmd_ota(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "arm") == 0) {
        unsigned seconds = argc == 3 ? (unsigned)atoi(argv[2]) : 300;
        if (argc > 4 || seconds == 0 || seconds > 600) return 1;
        if (argc >= 3) seconds = (unsigned)atoi(argv[2]);
        unsigned mask = OTA_ALLOW_ALL;
        if (argc == 4) {
            if (!strcmp(argv[3], "usb")) mask = OTA_ALLOW_USB;
            else if (!strcmp(argv[3], "wifi")) mask = OTA_ALLOW_WIFI;
            else if (!strcmp(argv[3], "ble")) mask = OTA_ALLOW_BLE;
            else if (strcmp(argv[3], "all")) return 1;
        }
        if (!seconds || seconds > 600) return 1;
        esp_err_t err = ble_ota_arm_transport(seconds, mask);
        if (err != ESP_OK) { printf("OTA arm failed: %s\n", esp_err_to_name(err)); return 1; }
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        esp_err_t err = ble_ota_arm(0);
        if (err != ESP_OK) { printf("OTA busy: %s\n", esp_err_to_name(err)); return 1; }
    } else if (argc > 2 || (argc == 2 && strcmp(argv[1], "status") != 0)) {
        printf("Usage: ota [status|arm [1..600] [usb|wifi|ble|all]|off]\n"); return 1;
    }
    uint8_t status[20];
    char info[512];
    ble_ota_status(status);
    ble_ota_info(info, sizeof(info));
    printf("OTA state=%u armed=%u busy=%u\n%s\n", status[1], status[3]&1,
           (status[3]>>1)&1, info);
    return 0;
}

static int cmd_wificfg(int argc, char **argv)
{
    if (argc < 3 || argc > 5) { printf("WLWIFI {\"ok\":false,\"error\":\"invalid_request\"}\n"); return 1; }
    char request[224] = {0};
    for (int i = 1; i < argc; ++i) {
        if (i > 1) strlcat(request, " ", sizeof(request));
        if (strlcat(request, argv[i], sizeof(request)) >= sizeof(request)) return 1;
    }
    esp_err_t err = wifi_setup_submit(request, true);
    memset(request, 0, sizeof(request));
    if (err != ESP_OK) {
        const char *error = err == ESP_ERR_INVALID_ARG ? "invalid_request" :
            err == ESP_ERR_INVALID_STATE ? "device_busy" : err == ESP_ERR_TIMEOUT ? "timeout" : "submit_failed";
        printf("WLWIFI {\"id\":\"%.8s\",\"ok\":false,\"error\":\"%s\"}\n", argv[1], error);
        return 1;
    }
    char response[512];
    for (int i = 0; i < 300; ++i) {
        wifi_setup_response(response, sizeof(response));
        if (!strstr(response, "\"busy\":true")) { printf("WLWIFI %s\n", response); return 0; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    printf("WLWIFI {\"id\":\"%.8s\",\"ok\":false,\"error\":\"timeout\"}\n", argv[1]);
    return 1;
}
static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1) {
        char ssid[33] = {0};
        char pass[65] = {0};
        bool ok = cfg_wifi_get(ssid, sizeof(ssid), pass, sizeof(pass));
        printf("WiFi STA: %s\n", ok ? ssid : "(未配置)");
        if (ok) {
            printf("SSID hex: ");
            for (size_t i = 0; i < strlen(ssid); ++i) {
                printf("%02x", (unsigned char)ssid[i]);
            }
            printf("\n");
        }
        printf("WiFi AP : %s\n", net_dev_ap_ip());
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        esp_err_t err = cfg_wifi_clear(500);
        if (err != ESP_OK) {
            printf("清除失败: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("已清除 WiFi 配置，正在重启…\n");
        return 0;
    }
    if (argc >= 2) {
        const char *pass = (argc >= 3) ? argv[2] : "";
        esp_err_t err = cfg_wifi_set(argv[1], pass, 500);
        if (err != ESP_OK) {
            printf("保存失败: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("已保存 WiFi: %s，正在重启…\n", argv[1]);
        return 0;
    }
    return 0;
}

/** 把十六进制字符串转成字节（用于中文/特殊字符 SSID） */
static int hex_to_bytes(const char *hex, uint8_t *out, size_t max)
{
    size_t len = strlen(hex);
    if (len < 2 || (len & 1) != 0) {
        return -1;
    }
    size_t n = len / 2;
    if (n > max) {
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        int vh = (hi >= '0' && hi <= '9') ? hi - '0'
                 : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                 : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
        int vl = (lo >= '0' && lo <= '9') ? lo - '0'
                 : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                 : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
        if (vh < 0 || vl < 0) {
            return -1;
        }
        out[i] = (uint8_t)((vh << 4) | vl);
    }
    return (int)n;
}

static int cmd_wifihex(int argc, char **argv)
{
    if (argc < 2) {
        printf("用法: wifihex <SSID的十六进制> [密码]\n");
        printf("例如: wifihex e7 8f 9e ...   (可用 powerShell 生成: [BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes('名字')) )\n");
        return 1;
    }
    char hex[3 * 33] = {0};
    /* 允许用空格或冒号分隔的十六进制 */
    size_t pos = 0;
    for (const char *p = argv[1]; *p && pos < sizeof(hex) - 1; ++p) {
        if (*p == ' ' || *p == ':' || *p == '-') {
            continue;
        }
        hex[pos++] = *p;
    }
    uint8_t ssid[33] = {0};
    int n = hex_to_bytes(hex, ssid, 32);
    if (n <= 0 || memchr(ssid, '\0', (size_t)n)) {
        printf("十六进制格式错误\n");
        return 1;
    }
    const char *pass = (argc >= 3) ? argv[2] : "";
    esp_err_t err = cfg_wifi_set((const char *)ssid, pass, 500);
    if (err != ESP_OK) {
        printf("保存失败: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("已保存 WiFi(%d 字节) 和密码，正在重启…\n", n);
    return 0;
}

static int cmd_swd(int argc, char **argv)
{
    if (argc >= 2) {
        int khz = atoi(argv[1]);
        if (khz < 100 || khz > 50000) {
            printf("范围 100 ~ 50000 kHz\n");
            return 1;
        }
        probe_set_clock((uint32_t)khz * 1000u);
    }
    printf("SWD 时钟: %" PRIu32 " kHz\n", probe_get_clock() / 1000u);
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const uint32_t speeds_fast[] = {1000, 500, 100, 50, 20, 10};
    static const uint32_t speeds_emu[] = {100, 50, 20};
    const bool emu = swd_emu_is_running();
    const uint32_t *speeds = emu ? speeds_emu : speeds_fast;
    const size_t speed_count = emu ? (sizeof(speeds_emu) / sizeof(speeds_emu[0]))
                                   : (sizeof(speeds_fast) / sizeof(speeds_fast[0]));
    const uint32_t old_hz = probe_get_clock();
    bool swapped = false;

    printf("连接目标…\n");
    for (size_t i = 0; i < speed_count; ++i) {
        probe_set_clock(speeds[i] * 1000u);
        probe_connect();
        if (probe_idcode() != 0) {
            printf("发现 SWD 目标, DP IDCODE = 0x%08" PRIx32 " (SWD %" PRIu32 " kHz)\n",
                   probe_idcode(), speeds[i]);
            goto found;
        }
        printf("  %" PRIu32 " kHz: 无应答，重试…\n", speeds[i]);
    }

    if (cfg_probe()->nreset_gpio >= 0) {
        printf("尝试 connect-under-reset…\n");
        probe_assert_reset(true);
        vTaskDelay(pdMS_TO_TICKS(50));
        probe_set_clock(200000);
        probe_connect();
        uint32_t id = probe_idcode();
        probe_assert_reset(false);
        if (id != 0) {
            printf(">>> 复位期间连上, DP IDCODE = 0x%08" PRIx32 "\n", id);
            goto found;
        }
        printf("复位期间仍无应答\n");
    }

    /* 再试一次：交换 SWCLK/SWDIO（目标侧接反的情况很常见） */
    printf("尝试交换 SWCLK/SWDIO 引脚…\n");
    probe_swap_pins();
    swapped = true;
    for (size_t i = 0; i < speed_count; ++i) {
        probe_set_clock(speeds[i] * 1000u);
        probe_connect();
        if (probe_idcode() != 0) {
            printf(">>> 交换引脚后连上了！DP IDCODE = 0x%08" PRIx32 " (SWD %" PRIu32 " kHz)\n",
                   probe_idcode(), speeds[i]);
            printf("提示：目标侧 SWCLK/SWDIO 接反了；诊断结束将恢复原映射，请修正接线\n");
            goto found;
        }
    }
    probe_swap_pins();   /* 恢复原引脚 */

    probe_set_clock(old_hz);
    printf("未读到 IDCODE。提示：\n");
    printf("  - 用 pins 看 SWDIO/SWCLK 电平是否随目标变化\n");
    printf("  - 确认目标 3.3V 供电、GND 共地、线没有松动\n");
    printf("  - 目标固件可能把 PA13/PA14 复用掉了，试试 swdreset\n");
    return 1;
found:
    if (swapped) probe_swap_pins();
    probe_set_clock(old_hz);
    printf("诊断完成，恢复原时钟 %" PRIu32 " kHz 和原引脚；交换引脚探测后的连接已释放\n", old_hz / 1000u);
    return 0;
}

/* ---- 数据通路自检：用 CLK 上升沿中断采样本机 SWDIO 输出 ----
 * 不需要任何外部接线：读到什么就说明发出去的是什么。 */
static volatile uint8_t s_cap[64];
static volatile int     s_cap_n;

static void IRAM_ATTR cap_isr(void *arg)
{
    (void)arg;
    const int clk = cfg_probe()->swclk_gpio;
    const int dio = cfg_probe()->swdio_gpio;
    if ((REG_READ(GPIO_IN_REG) & (1u << clk)) == 0) {
        return;                 /* 只在上升沿采样 */
    }
    if (s_cap_n < (int)sizeof(s_cap)) {
        s_cap[s_cap_n++] = (uint8_t)((REG_READ(GPIO_IN_REG) >> dio) & 1u);
    }
}

static volatile uint8_t s_trace[512];
static volatile int     s_trace_n;

static void IRAM_ATTR trace_isr(void *arg)
{
    (void)arg;
    const int clk = cfg_probe()->swclk_gpio;
    const int dio = cfg_probe()->swdio_gpio;
    if ((REG_READ(GPIO_IN_REG) & (1u << clk)) == 0) {
        return;
    }
    if (s_trace_n < (int)sizeof(s_trace)) {
        s_trace[s_trace_n++] = (uint8_t)((REG_READ(GPIO_IN_REG) >> dio) & 1u);
    }
}

static int cmd_trace(int argc, char **argv)
{
    (void)argc; (void)argv;
    const int clk = cfg_probe()->swclk_gpio;
    (void)gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_set_intr_type(clk, GPIO_INTR_POSEDGE);
    gpio_isr_handler_add(clk, trace_isr, NULL);
    gpio_intr_enable(clk);

    s_trace_n = 0;
    probe_lock();
    probe_connect();
    probe_unlock();

    gpio_intr_disable(clk);
    gpio_isr_handler_remove(clk);

    int n = s_trace_n;
    printf("共采到 %d 个上升沿（目标看到的位流）:\n", n);
    for (int i = 0; i < n && i < 320; ++i) {
        printf("%d", s_trace[i]);
        if ((i % 8) == 7) printf(" ");
        if ((i % 64) == 63) printf("\n");
    }
    printf("\n");
    return 0;
}

static int cmd_datachk(int argc, char **argv)
{
    uint32_t byte = (argc >= 2) ? (uint32_t)strtoul(argv[1], NULL, 16) : 0xA5u;
    const int clk = cfg_probe()->swclk_gpio;

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("ISR 服务安装失败: %s\n", esp_err_to_name(err));
        return 1;
    }
    gpio_set_intr_type(clk, GPIO_INTR_POSEDGE);
    gpio_isr_handler_add(clk, cap_isr, NULL);
    gpio_intr_enable(clk);

    s_cap_n = 0;
    probe_lock();
    uint8_t d = (uint8_t)byte;
    probe_swd_output_bits(8, &d);
    probe_unlock();

    gpio_intr_disable(clk);
    gpio_isr_handler_remove(clk);

    printf("发送 0x%02X，CLK 上升沿采到 %d 位: ", (unsigned)byte, s_cap_n);
    int n = s_cap_n;
    for (int i = 0; i < n; ++i) {
        printf("%d", s_cap[i]);
    }
    printf("\n");
    if (n >= 8) {
        uint32_t got = 0;
        for (int i = 0; i < 8; ++i) {
            got |= (uint32_t)s_cap[n - 8 + i] << i;
        }
        printf("按 LSB-first 还原: 0x%02X %s\n", (unsigned)got,
               (got == byte) ? "✅ 一致" : "❌ 不一致");
    } else if (n == 0) {
        printf("❌ 一个上升沿都没采到：CLK 根本没有翻转\n");
    }
    return 0;
}

static int cmd_swdemu(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        swd_emu_start();
        printf("仿真器已启动（SWD 时钟已设 100kHz）。接线：GPIO4->GPIO7, GPIO5<->GPIO8，然后执行 scan\n");
    } else if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        swd_emu_stop();
        printf("仿真器已停止\n");
    } else if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        uint8_t buf[16];
        uint32_t total = 0, valid = 0, reads = 0;
        swd_emu_dump(buf, sizeof(buf), &total, &valid, &reads);
        printf("收到 8 位组 %" PRIu32 "，合法请求 %" PRIu32 "，完成读事务 %" PRIu32 "\n",
               total, valid, reads);
        printf("最近组: ");
        uint32_t n = (total < sizeof(buf)) ? total : (uint32_t)sizeof(buf);
        for (uint32_t i = 0; i < n; ++i) {
            printf("%02X ", buf[i]);
        }
        printf("\n");
    } else {
        printf("用法: swdemu on|off|dump\n");
    }
    return 0;
}

static int cmd_swdtr(int argc, char **argv)
{
    if (argc >= 2) {
        probe_set_turnaround((uint32_t)atoi(argv[1]));
    }
    printf("turnaround 已设置\n");
    return 0;
}

static void print_phase_timing(const char *name, const swd_phase_timing_t *phase,
                               uint32_t cpu_mhz)
{
    double mean_mhz = phase->total_cycles
        ? 32.0 * cpu_mhz * phase->samples / (double)phase->total_cycles : 0.0;
    double fastest_mhz = phase->min_cycles ? 32.0 * cpu_mhz / phase->min_cycles : 0.0;
    printf("SWD_TIMING %s samples=%" PRIu32 " min_cycles=%" PRIu32
           " max_cycles=%" PRIu32 " total_cycles=%" PRIu64
           " mean_mhz=%.6f fastest_mhz=%.6f\n", name, phase->samples,
           phase->min_cycles, phase->max_cycles, phase->total_cycles,
           mean_mhz, fastest_mhz);
}

static int cmd_swdtiming(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "reset") == 0) {
            probe_swd_timing_reset(true);
        } else if (strcmp(argv[1], "off") == 0) {
            probe_swd_timing_reset(false);
        } else {
            printf("Usage: swdtiming [reset|off]\n");
            return 1;
        }
    }
    swd_timing_stats_t stats;
    probe_swd_timing_get(&stats);
    printf("SWD_TIMING enabled=%d requested_hz=%" PRIu32 " half_nops=%" PRIu32
           " cpu_mhz=%" PRIu32 " fast_enabled=%d dedicated_ready=%d fast_low_nops=%" PRIu32
           " spi_enabled=%d spi_selected=%d spi_hz=%" PRIu32 "\n",
           stats.enabled, stats.requested_hz, stats.half_nops, stats.cpu_mhz,
           stats.fast_enabled, stats.dedicated_ready, stats.fast_low_nops,
           stats.spi_enabled, stats.spi_selected, stats.spi_hz);
    print_phase_timing("read", &stats.read, stats.cpu_mhz);
    print_phase_timing("write", &stats.write, stats.cpu_mhz);
    printf("SWD_ACK ok=%" PRIu32 " wait=%" PRIu32 " fault=%" PRIu32
           " invalid=%" PRIu32 " parity_errors=%" PRIu32 "\n",
           stats.acks[1], stats.acks[2], stats.acks[4],
           stats.acks[0] + stats.acks[3] + stats.acks[5] + stats.acks[6] + stats.acks[7],
           stats.parity_errors);
    return 0;
}

static int cmd_swdspi(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        esp_err_t err = probe_swd_set_spi(true);
        if (err != ESP_OK) {
            printf("SPI2 enable failed: %s\n", esp_err_to_name(err));
            return 1;
        }
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        (void)probe_swd_set_spi(false);
    } else if (argc == 3 && strcmp(argv[1], "align") == 0) {
        if (strcmp(argv[2], "auto") == 0) swd_spi_set_alignment(-1);
        else if (strcmp(argv[2], "0") == 0) swd_spi_set_alignment(0);
        else if (strcmp(argv[2], "1") == 0) swd_spi_set_alignment(1);
        else return 1;
    } else if (argc > 2 || (argc == 2 && strcmp(argv[1], "status") != 0)) {
        printf("Usage: swdspi [on|off|status|align auto|align 0|align 1]\n");
        return 1;
    }
    swd_spi_stats_t spi;
    swd_spi_get_stats(&spi);
    printf("SWD_SPI ready=%d actual_hz=%" PRIu32 " alignment=%d rx_delayed=%d calibrated=%d transfers=%" PRIu32
           " timeouts=%" PRIu32 " parity_errors=%" PRIu32 " tx_bits=%" PRIu64
           " rx_bits=%" PRIu64 " last_request=0x%02" PRIx32 " ack_raw=0x%08" PRIx32
           " rx_low=0x%08" PRIx32 " rx_high=0x%08" PRIx32 "\n",
           spi.ready, spi.actual_hz, spi.alignment, spi.rx_delayed, spi.calibrated,
           spi.transfers, spi.timeouts,
           spi.parity_errors, spi.tx_bits, spi.rx_bits, spi.last_request,
           spi.ack_raw, spi.rx_low, spi.rx_high);
    return cmd_swdtiming(1, argv);
}

static int cmd_swdfast(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: swdfast off|0..32 (dedicated GPIO low-phase NOP count)\n");
        return 1;
    }
    if (strcmp(argv[1], "off") == 0) {
        probe_swd_set_fast_padding(-1);
    } else {
        char *end;
        long padding = strtol(argv[1], &end, 10);
        if (*end || padding < 0 || padding > 32) {
            return 1;
        }
        probe_swd_set_fast_padding((int)padding);
    }
    return cmd_swdtiming(1, argv);
}

static int cmd_swdclkidle(int argc, char **argv)
{
    if (argc >= 2) {
        int v = atoi(argv[1]);
        probe_swd_set_clk_idle_low(v);
        printf("SWCLK 空闲电平: %s\n", v ? "低" : "高");
    }
    return 0;
}

static int cmd_swdidle(int argc, char **argv)
{
    if (argc >= 2) {
        int n = atoi(argv[1]);
        if (n < 0) {
            n = 0;
        }
        probe_set_idle_cycles((uint8_t)(n > 255 ? 255 : n));
        printf("idle 时钟数: %d\n", n);
    }
    return 0;
}

static int cmd_nrsttest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t us = probe_swd_nrst_rise_us();
    if (us == 0xFFFFFFFFu) {
        printf("未配置 nRESET 引脚\n");
        return 1;
    }
    if (us == 0xFFFFFFFEu) {
        printf("释放 NRST 后一直没升高（可能被外部强拉低）\n");
        return 1;
    }
    printf("NRST 释放后上升时间: %" PRIu32 " us\n", us);
    if (us < 20) {
        printf(">>> 上升太快，目标板 NRST 上通常有 100nF，怀疑 NRST 线没接到目标\n");
    } else {
        printf(">>> 有 RC 充电特征，NRST 线应该接到了目标板\n");
    }
    return 0;
}

static int cmd_iochk(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int r = probe_swd_release_test();
    printf("SWDIO 驱动高时回读  : %d (应为 1)\n", r & 1);
    printf("SWDIO 释放+下拉后    : %d (应为 0；1 表示输出使能没关掉)\n", (r >> 1) & 1);
    printf("SWCLK 驱动高时回读  : %d (应为 1；0 表示 CLK 线被拉死/驱动失效)\n", (r >> 2) & 1);
    printf("SWCLK 驱动低时回读  : %d (应为 0；1 表示 CLK 线被外部拉高)\n", (r >> 3) & 1);
    return 0;
}

static int cmd_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int du = 0, dd = 0, cu = 0, cd = 0, nr = 0;
    probe_diag(&du, &dd, &cu, &cd, &nr);
    int mv_dio = -1, mv_clk = -1, mv_nrst = -1;
    probe_diag_mv(&mv_dio, &mv_clk, &mv_nrst);

    printf("SWDIO 内部上拉时: %d\n", du);
    printf("SWDIO 内部下拉时: %d   (目标板 SWDIO 通常有 10k 上拉，接了目标会读到 1)\n", dd);
    printf("SWCLK 内部上拉时: %d\n", cu);
    printf("SWCLK 内部下拉时: %d\n", cd);
    printf("nRESET 电平     : %d   (-1 = 未配置)\n", nr);
    printf("SWDIO 下拉时电压: %d mV  (接了目标约 1500~1800mV；nl 约 0mV)\n", mv_dio);
    printf("SWCLK 上拉时电压: %d mV  (接了目标约 1500~1800mV；无目标约 3100mV)\n", mv_clk);
    printf("nRESET 电压     : %d mV\n", mv_nrst);
    if (mv_dio >= 0 && mv_dio < 300) {
        printf(">>> SWDIO 上看不到目标板的上拉，请检查 SWDIO 是否真的接到 GPIO%d\n",
               cfg_probe()->swdio_gpio);
    }
    if (mv_clk > 2600) {
        printf(">>> SWCLK 上拉时接近满幅，目标的 SWCLK(内部下拉) 可能没接上 GPIO%d\n",
               cfg_probe()->swclk_gpio);
    }
    return 0;
}

static int cmd_swdreset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (cfg_probe()->nreset_gpio < 0) {
        printf("没有配置 nRESET 引脚，无法做 connect-under-reset\n");
        return 1;
    }
    printf("拉低 NRST 并保持，尝试在复位期间连接…\n");
    probe_assert_reset(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    probe_connect();
    uint32_t id = probe_idcode();
    if (id) {
        printf(">>> 复位期间连上了！DP IDCODE = 0x%08" PRIx32 "\n", id);
    } else {
        printf("复位期间仍未读到 IDCODE\n");
    }
    probe_assert_reset(false);
    vTaskDelay(pdMS_TO_TICKS(50));
    probe_connect();
    printf("释放复位后 DP IDCODE = 0x%08" PRIx32 "\n", probe_idcode());
    return id ? 0 : 1;
}

static int cmd_rstlow(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    probe_assert_reset(true);
    printf("NRST 已拉低（保持），用 rsthi 释放\n");
    return 0;
}

static int cmd_rsthi(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    probe_assert_reset(false);
    printf("NRST 已释放\n");
    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    probe_status_t status = probe_reset_target();
    if (status != PROBE_OK) {
        printf("目标复位失败（状态 %d）；请连接并配置 nRESET\n", status);
        return 1;
    }
    printf("已复位目标\n");
    return 0;
}

static int cmd_baud(int argc, char **argv)
{
    if (argc >= 2) {
        int baud = atoi(argv[1]);
        if (baud < 300 || baud > 4000000) {
            printf("波特率范围 300 ~ 4000000\n");
            return 1;
        }
        serial_bridge_set_baud(baud);
    }
    printf("目标串口: %d baud\n", serial_bridge_get_baud());
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = cfg_reboot_later(200);
    if (err != ESP_OK) {
        printf("重启请求失败: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("重启中…\n");
    return 0;
}

/* Optional scratch RAM is supplied by the operator; default selftest only
 * reads AP IDR. Back up before writing and attempt restoration on any error. */
static int cmd_selftest(int argc, char **argv)
{
    if (argc < 2 || argc > 3) { printf("Usage: selftest <APSEL> [halted-target-scratch-RAM-address]\n"); return 1; }
    char *end;
    unsigned long ap = strtoul(argv[1], &end, 0);
    if (*end || ap > 255) return 1;
    probe_status_t st = probe_mem_select_ap((uint32_t)ap, 0xf0);
    uint32_t idr = 0;
    if (st == PROBE_OK) st = probe_ap_read(0xfc, &idr);
    printf("AP%lu IDR=0x%08" PRIx32 " status=%d\n", ap, idr, st);
    if (st != PROBE_OK || !idr || idr == UINT32_MAX) return 1;
    if (argc == 2) return 0;
    unsigned long addr = strtoul(argv[2], &end, 0);
    if (*end || addr > UINT32_MAX - 31u || (addr & 3u)) return 1;
    uint32_t saved[8], pattern[8], actual[8];
    st = probe_mem_select_ap((uint32_t)ap, 0);
    if (st == PROBE_OK) st = probe_mem_read_block32((uint32_t)addr, saved, 8);
    if (st != PROBE_OK) { printf("Scratch backup failed, no writes performed\n"); return 1; }
    for (unsigned i = 0; i < 8; ++i) pattern[i] = 0xa5a50000u ^ (0x10204081u * i);
    st = probe_mem_write_block32((uint32_t)addr, pattern, 8);
    if (st == PROBE_OK) st = probe_mem_read_block32((uint32_t)addr, actual, 8);
    bool passed = st == PROBE_OK && !memcmp(pattern, actual, sizeof(pattern));
    probe_status_t restored = probe_mem_write_block32((uint32_t)addr, saved, 8);
    if (restored == PROBE_OK) restored = probe_mem_read_block32((uint32_t)addr, actual, 8);
    bool restore_ok = restored == PROBE_OK && !memcmp(saved, actual, sizeof(saved));
    printf("RAM loopback=%s restore=%s\n", passed ? "PASS" : "FAIL", restore_ok ? "PASS" : "FAIL");
    return passed && restore_ok ? 0 : 1;
}

/* 会直接碰 SWD 总线的命令统一加锁，避免和 dap_worker 撞车 */
#define LOCKED_WRAP(name, fn)                                             \
    static int name(int argc, char **argv)                                \
    {                                                                    \
        probe_lock();                                                    \
        if (link_swd_session_active()) {                                 \
            probe_unlock();                                              \
            printf("SWD 正被调试会话占用\n");                            \
            return 1;                                                    \
        }                                                                \
        int r = fn(argc, argv);                                          \
        probe_unlock();                                                  \
        return r;                                                        \
    }

LOCKED_WRAP(cmd_selftest_l, cmd_selftest)
LOCKED_WRAP(cmd_swdtr_l, cmd_swdtr)
LOCKED_WRAP(cmd_swdidle_l, cmd_swdidle)
LOCKED_WRAP(cmd_swdclkidle_l, cmd_swdclkidle)
LOCKED_WRAP(cmd_swdemu_l, cmd_swdemu)
LOCKED_WRAP(cmd_datachk_l, cmd_datachk)
LOCKED_WRAP(cmd_trace_l, cmd_trace)
LOCKED_WRAP(cmd_rstlow_l, cmd_rstlow)
LOCKED_WRAP(cmd_rsthi_l, cmd_rsthi)
LOCKED_WRAP(cmd_swd_l, cmd_swd)
LOCKED_WRAP(cmd_swdtiming_l, cmd_swdtiming)
LOCKED_WRAP(cmd_swdfast_l, cmd_swdfast)
LOCKED_WRAP(cmd_swdspi_l, cmd_swdspi)
LOCKED_WRAP(cmd_scan_l, cmd_scan)
LOCKED_WRAP(cmd_pins_l, cmd_pins)
LOCKED_WRAP(cmd_iochk_l, cmd_iochk)
LOCKED_WRAP(cmd_nrsttest_l, cmd_nrsttest)
LOCKED_WRAP(cmd_swdreset_l, cmd_swdreset)
LOCKED_WRAP(cmd_reset_l, cmd_reset)

static const esp_console_cmd_t s_cmds[] = {
    {.command = "wificfg", .help = "WiFi setup protocol: id status|scan|page|save|clear", .func = cmd_wificfg},
    {.command = "selftest", .help = "selftest <APSEL> [scratch RAM] AP IDR and optional RAM loopback", .func = cmd_selftest_l},
    {.command = "status", .help = "显示 USB/WiFi/BLE/SWD 状态", .func = cmd_status},
    {.command = "ota", .help = "ota status|arm [seconds] [usb|wifi|ble|all]|off ESP32 USB/WiFi/BLE 自身固件升级", .func = cmd_ota},
    {.command = "wifi", .help = "wifi <ssid> <pass> 设置 | wifi -c 清除 | wifi 查看", .func = cmd_wifi},
    {.command = "wifihex", .help = "wifihex <SSID十六进制> [密码]（支持中文 SSID）", .func = cmd_wifihex},
    {.command = "swd", .help = "swd [kHz] 设置 SWD 时钟", .func = cmd_swd_l},
    {.command = "swdtiming", .help = "swdtiming [reset|off] 数据段计时与 ACK 统计", .func = cmd_swdtiming_l},
    {.command = "swdfast", .help = "swdfast off|0..32 专用 GPIO 及低电平 NOP", .func = cmd_swdfast_l},
    {.command = "swdspi", .help = "swdspi on|off|status|align auto|align 0|align 1", .func = cmd_swdspi_l},
    {.command = "scan", .help = "扫描 SWD 目标（读 IDCODE）", .func = cmd_scan_l},
    {.command = "pins", .help = "SWD 引脚电气诊断", .func = cmd_pins_l},
    {.command = "iochk", .help = "验证 SWDIO 释放(tri-state)与 SWCLK 输出", .func = cmd_iochk_l},
    {.command = "nrsttest", .help = "测 NRST 线是否真的接到目标", .func = cmd_nrsttest_l},
    {.command = "swdtr", .help = "swdtr <0|1> 请求后额外 turnaround 时钟数", .func = cmd_swdtr_l},
    {.command = "swdidle", .help = "swdidle <n> 传输末尾 idle 时钟数", .func = cmd_swdidle_l},
    {.command = "swdclkidle", .help = "swdclkidle <0|1> SWCLK 空闲电平", .func = cmd_swdclkidle_l},
    {.command = "swdemu", .help = "swdemu on|off|dump 内置 SWD 目标仿真器（自检）", .func = cmd_swdemu_l},
    {.command = "datachk", .help = "datachk [hex] 用 CLK 中断回采 SWDIO 输出，验证波形", .func = cmd_datachk_l},
    {.command = "trace", .help = "trace 录一次 probe_connect 的完整位流", .func = cmd_trace_l},
    {.command = "rst", .help = "复位目标", .func = cmd_reset_l},
    {.command = "swdreset", .help = "复位期间连接（connect under reset）", .func = cmd_swdreset_l},
    {.command = "rstlow", .help = "拉低 NRST 并保持", .func = cmd_rstlow_l},
    {.command = "rsthi", .help = "释放 NRST", .func = cmd_rsthi_l},
    {.command = "baud", .help = "baud [n] 设置目标串口波特率", .func = cmd_baud},
    {.command = "reboot", .help = "重启设备", .func = cmd_reboot},
};

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "esp-swd>";
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.task_stack_size = 5120;
    repl_cfg.task_core_id = 1;  /* 与 dap_worker、专用 GPIO 的所属 CPU 一致 */

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE("console", "初始化控制台失败: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); ++i) {
        esp_console_cmd_register(&s_cmds[i]);
    }
    err = esp_console_start_repl(repl);
    if (err != ESP_OK) return err;
    ESP_LOGI("console", "控制台已启动，输入 help 查看命令");
    return ESP_OK;
}
