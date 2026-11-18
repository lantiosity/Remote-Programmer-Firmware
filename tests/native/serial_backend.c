#include <stdlib.h>

#define WIRELINK_SERIAL_TEST
#include "serial_bridge.c"

typedef struct {
    unsigned capacity, item_size, count;
    uint8_t items[];
} test_queue_t;

QueueHandle_t xQueueCreate(unsigned depth, unsigned size)
{
    test_queue_t *q = calloc(1, sizeof(*q) + depth * size);
    if (q) { q->capacity = depth; q->item_size = size; }
    return q;
}

void vQueueDelete(QueueHandle_t queue) { free(queue); }

int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    test_queue_t *q = queue;
    if (q->count == q->capacity) return 0;
    memcpy(q->items + q->count * q->item_size, item, q->item_size);
    ++q->count;
    return pdTRUE;
}

int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    test_queue_t *q = queue;
    if (!q->count) return 0;
    memcpy(item, q->items, q->item_size);
    --q->count;
    memmove(q->items, q->items + q->item_size, q->count * q->item_size);
    return pdTRUE;
}

unsigned mock_lock_errors, mock_locks_held;
unsigned mock_cdc_reads, mock_cdc_writes, mock_uart_writes, mock_sink_calls;
static serial_input_chunk_t pending;

int cfg_uart_port(void) { return 1; }
int cfg_uart_tx_gpio(void) { return 17; }
int cfg_uart_rx_gpio(void) { return 18; }
int cfg_uart_baud(void) { return 115200; }
esp_err_t cfg_uart_set_baud(int baud) { return 0; }
void usb_dev_cdc_write(const uint8_t *data, size_t len) { ++mock_cdc_writes; }
size_t usb_dev_cdc_read(uint8_t *data, size_t len) { data[0] = 0xa0 + ++mock_cdc_reads; return 1; }
int uart_write_bytes(int port, const char *data, size_t len) { ++mock_uart_writes; return (int)len; }
int uart_read_bytes(int port, void *data, size_t len, unsigned ticks) { *(uint8_t *)data = 0x5a; return 1; }
const char *esp_err_to_name(esp_err_t err) { return "mock"; }
static bool sink(int token, const uint8_t *data, size_t len) { ++mock_sink_calls; return true; }
int test_subscribe(int token) { return serial_bridge_subscribe(sink, token); }
void test_unsubscribe(int token) { serial_bridge_unsubscribe(sink, token); }

int test_start_uart_tx(void)
{
    pending = (serial_input_chunk_t){0};
    mock_cdc_reads = mock_cdc_writes = mock_uart_writes = mock_sink_calls = 0;
    return serial_bridge_start();
}

int test_queue_uart_byte(uint8_t byte)
{
    const serial_input_chunk_t chunk = {.len = 1, .data = {byte}};
    return serial_bridge_to_target(&chunk, 0);
}

int test_dequeue_uart_byte(void)
{
    serial_input_chunk_t chunk;
    if (xQueueReceive(s_uart_tx, &chunk, 0) != pdTRUE || chunk.len != 1) return -1;
    return chunk.data[0];
}

void test_serial_rx_step(void) { serial_rx_step(&pending); }
