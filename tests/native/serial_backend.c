#include "serial_bridge.h"
unsigned mock_lock_errors, mock_locks_held;
int cfg_uart_port(void) { return 1; }
int cfg_uart_tx_gpio(void) { return 17; }
int cfg_uart_rx_gpio(void) { return 18; }
int cfg_uart_baud(void) { return 115200; }
esp_err_t cfg_uart_set_baud(int baud) { return 0; }
void usb_dev_cdc_write(const uint8_t *data, size_t len) { }
size_t usb_dev_cdc_read(uint8_t *data, size_t len) { return 0; }
const char *esp_err_to_name(esp_err_t err) { return "mock"; }
static bool sink(int token, const uint8_t *data, size_t len) { return true; }
int test_subscribe(int token) { return serial_bridge_subscribe(sink, token); }
void test_unsubscribe(int token) { serial_bridge_unsubscribe(sink, token); }
