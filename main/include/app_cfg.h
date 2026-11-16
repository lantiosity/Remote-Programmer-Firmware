/**
 * @file  app_cfg.h
 * @brief 运行时配置：引脚、WiFi 凭据（NVS 覆盖 Kconfig 默认值）、设备标识。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SWD 相关引脚配置 */
typedef struct {
    int      swclk_gpio;
    int      swdio_gpio;
    int      nreset_gpio;   /**< -1 表示未接 */
    uint32_t clock_hz;      /**< 默认 SWD 时钟 */
} cfg_probe_pins_t;

/** 初始化配置模块（需要 nvs_flash 已初始化） */
esp_err_t cfg_init(void);

const cfg_probe_pins_t *cfg_probe(void);

/** 串口（目标 UART）配置 */
int      cfg_uart_port(void);
int      cfg_uart_tx_gpio(void);
int      cfg_uart_rx_gpio(void);
int      cfg_uart_baud(void);
esp_err_t cfg_uart_set_baud(int baud);

/** WiFi 凭据（优先返回 NVS 中的值，其次 Kconfig 默认值） */
bool cfg_wifi_get(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
esp_err_t cfg_wifi_set(const char *ssid, const char *pass);
esp_err_t cfg_wifi_clear(void);

/** 设备标识 */
const char *cfg_serial_number(void);
const char *cfg_ble_name(void);

/** 网络参数 */
uint16_t cfg_dap_tcp_port(void);
uint16_t cfg_serial_tcp_port(void);

/** 重启（延时后） */
void cfg_reboot_later(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
