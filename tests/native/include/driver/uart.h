#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int uart_port_t;
#define UART_NUM_1 1
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
typedef struct { int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk; } uart_config_t;
static inline int uart_driver_install(int p,int rx,int tx,int n,void *q,int flags) { return 0; }
static inline int uart_param_config(int p,const uart_config_t *c) { return 0; }
static inline int uart_set_pin(int p,int tx,int rx,int rts,int cts) { return 0; }
static inline int uart_write_bytes(int p,const char *data,size_t len) { return (int)len; }
static inline int uart_read_bytes(int p,void *data,size_t len,unsigned ticks) { return 0; }
static inline int uart_set_baudrate(int p,unsigned baud) { return 0; }
