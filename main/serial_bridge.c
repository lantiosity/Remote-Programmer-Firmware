/**
 * @file  serial_bridge.c
 * @brief  目标串口桥：UART <-> USB CDC / BLE NUS / TCP
 */
#include <string.h>
#include <stdatomic.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_cfg.h"
#include "serial_bridge.h"
#include "usb_dev.h"

static const char *TAG = "serial";

#define SINK_MAX 4
#define SERIAL_INPUT_QUEUE_DEPTH 4

typedef struct {
    bool              used;
    serial_sink_fn_t  fn;
    int               opaque;
} sink_t;

static sink_t     s_sinks[SINK_MAX];
static SemaphoreHandle_t s_sinks_mtx;
static QueueHandle_t s_uart_tx;
static uart_port_t s_port = UART_NUM_1;
static int         s_baud = 115200;
static atomic_uint s_dropped_bytes[SERIAL_OUTPUT_COUNT];

void serial_bridge_note_drop(serial_output_t output, size_t bytes)
{
    atomic_fetch_add_explicit(&s_dropped_bytes[output], (unsigned)bytes, memory_order_relaxed);
}

uint32_t serial_bridge_dropped_bytes(serial_output_t output)
{
    return atomic_load_explicit(&s_dropped_bytes[output], memory_order_relaxed);
}

esp_err_t serial_bridge_init(void)
{
    if (!s_sinks_mtx) s_sinks_mtx = xSemaphoreCreateMutex();
    if (!s_sinks_mtx) return ESP_ERR_NO_MEM;
    s_port = (uart_port_t)cfg_uart_port();
    s_baud = cfg_uart_baud();

    const uart_config_t cfg = {
        .baud_rate = s_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(s_port, 4096, 4096, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(s_port, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(s_port, cfg_uart_tx_gpio(), cfg_uart_rx_gpio(),
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "目标串口 UART%d TX=GPIO%d RX=GPIO%d @%d",
             (int)s_port, cfg_uart_tx_gpio(), cfg_uart_rx_gpio(), s_baud);
    return ESP_OK;
}

esp_err_t serial_bridge_to_target(const serial_input_chunk_t *chunk, TickType_t timeout)
{
    if (!chunk || !chunk->len || chunk->len > sizeof(chunk->data)) return ESP_ERR_INVALID_ARG;
    if (!s_uart_tx) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_uart_tx, chunk, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t serial_bridge_subscribe(serial_sink_fn_t fn, int opaque)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sinks_mtx) return ESP_ERR_INVALID_STATE;
    (void)xSemaphoreTake(s_sinks_mtx, portMAX_DELAY);
    for (int i = 0; i < SINK_MAX; ++i) {
        if (s_sinks[i].used && s_sinks[i].fn == fn && s_sinks[i].opaque == opaque) {
            xSemaphoreGive(s_sinks_mtx);
            return ESP_OK;
        }
    }
    for (int i = 0; i < SINK_MAX; ++i) {
        if (!s_sinks[i].used) {
            s_sinks[i].used = true;
            s_sinks[i].fn = fn;
            s_sinks[i].opaque = opaque;
            if (s_sinks_mtx) {
                (void)xSemaphoreGive(s_sinks_mtx);
            }
            return ESP_OK;
        }
    }
    if (s_sinks_mtx) {
        (void)xSemaphoreGive(s_sinks_mtx);
    }
    return ESP_ERR_NO_MEM;
}

void serial_bridge_unsubscribe(serial_sink_fn_t fn, int opaque)
{
    if (!s_sinks_mtx) return;
    (void)xSemaphoreTake(s_sinks_mtx, portMAX_DELAY);
    for (int i = 0; i < SINK_MAX; ++i) {
        if (s_sinks[i].used && s_sinks[i].fn == fn && s_sinks[i].opaque == opaque) {
            s_sinks[i].used = false;
            s_sinks[i].fn = NULL;
            break;
        }
    }
    xSemaphoreGive(s_sinks_mtx);
}

esp_err_t serial_bridge_set_baud(int baud)
{
    if (baud <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_baud = baud;
    cfg_uart_set_baud(baud);
    return uart_set_baudrate(s_port, (uint32_t)baud);
}

int serial_bridge_get_baud(void)
{
    return s_baud;
}

static void serial_rx_step(serial_input_chunk_t *pending)
{
    uint8_t buf[SERIAL_BRIDGE_CHUNK_SIZE];
    int n = uart_read_bytes(s_port, buf, sizeof(buf), pdMS_TO_TICKS(10));
    if (n > 0) {
        usb_dev_cdc_write(buf, (size_t)n);
        serial_sink_fn_t fns[SINK_MAX];
        int opaques[SINK_MAX];
        int count = 0;
        if (xSemaphoreTake(s_sinks_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (int i = 0; i < SINK_MAX; ++i) {
                if (s_sinks[i].used) {
                    fns[count] = s_sinks[i].fn;
                    opaques[count] = s_sinks[i].opaque;
                    ++count;
                }
            }
            xSemaphoreGive(s_sinks_mtx);
        }
        for (int i = 0; i < count; ++i) fns[i](opaques[i], buf, (size_t)n);
    }

    /* Keep one CDC chunk on congestion so the USB RX FIFO applies backpressure. */
    if (!pending->len) pending->len = usb_dev_cdc_read(pending->data, sizeof(pending->data));
    if (pending->len && serial_bridge_to_target(pending, 0) == ESP_OK) pending->len = 0;
}

static void serial_task(void *arg)
{
    (void)arg;
    serial_input_chunk_t pending = {0};
    for (;;) serial_rx_step(&pending);
}

static void uart_tx_task(void *arg)
{
    (void)arg;
    serial_input_chunk_t chunk;
    for (;;) {
        if (xQueueReceive(s_uart_tx, &chunk, portMAX_DELAY) != pdTRUE) continue;
        if (uart_write_bytes(s_port, (const char *)chunk.data, chunk.len) != (int)chunk.len)
            ESP_LOGE(TAG, "目标 UART 写入失败 (%u 字节)", (unsigned)chunk.len);
    }
}

esp_err_t serial_bridge_start(void)
{
    if (!s_sinks_mtx) return ESP_ERR_INVALID_STATE;
    if (s_uart_tx) return ESP_OK;
    s_uart_tx = xQueueCreate(SERIAL_INPUT_QUEUE_DEPTH, sizeof(serial_input_chunk_t));
    if (!s_uart_tx) return ESP_ERR_NO_MEM;
    TaskHandle_t tx_task;
    if (xTaskCreate(uart_tx_task, "uart_tx", 3072, NULL, 8, &tx_task) == pdPASS) {
        if (xTaskCreate(serial_task, "serial_bridge", 4096, NULL, 8, NULL) == pdPASS) return ESP_OK;
        vTaskDelete(tx_task);
    }
    vQueueDelete(s_uart_tx);
    s_uart_tx = NULL;
    return ESP_ERR_NO_MEM;
}
