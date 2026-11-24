/* USB HID and WiFi TCP adapters for the shared OTA worker.
 * TCP :5557: <4sBBH> = "WLO1", kind, reserved=0, payload length (LE).
 * Reply kind|0x80, payload = esp_err_t:u32 + result. One client at a time.
 * Kinds: 1 control, 2 data, 3 status, 4 info (optional offset:u16), 5 arm
 * (USB only, seconds:u16). Control/data match the BLE protocol.
 */
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "ble_ota.h"
#include "wired_ota.h"
#include "wifi_setup.h"
#include "sdkconfig.h"
static char s_wifi_rx[224];
static size_t s_wifi_rx_len;

static const char *TAG = "wired_ota";
static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8 * i);
}

static esp_err_t request(uint16_t owner, uint8_t kind, const uint8_t *p, size_t n,
                         uint8_t *out, size_t capacity, size_t *used)
{
    *used = 0;
    esp_err_t err;
    if (kind == 6) {
        if (owner != OTA_OWNER_USB) return ESP_ERR_NOT_ALLOWED;
        if (n < 2 || p[0] > 1) return ESP_ERR_INVALID_ARG;
        if (p[0]) { memset(s_wifi_rx, 0, sizeof(s_wifi_rx)); s_wifi_rx_len = 0; }
        if (s_wifi_rx_len + n - 1 >= sizeof(s_wifi_rx)) { s_wifi_rx_len = 0; return ESP_ERR_INVALID_SIZE; }
        memcpy(s_wifi_rx + s_wifi_rx_len, p + 1, n - 1); s_wifi_rx_len += n - 1;
        if (s_wifi_rx[s_wifi_rx_len - 1] == '\n') {
            s_wifi_rx[--s_wifi_rx_len] = 0;
            err = wifi_setup_submit(s_wifi_rx, true);
            memset(s_wifi_rx, 0, sizeof(s_wifi_rx)); s_wifi_rx_len = 0;
            return err;
        }
        return ESP_OK;
    }
    if (kind == 7) {
        if (owner != OTA_OWNER_USB || n != 2) return ESP_ERR_INVALID_ARG;
        char json[512]; size_t length = wifi_setup_response(json, sizeof(json));
        size_t offset = get16(p);
        if (offset > length) return ESP_ERR_INVALID_ARG;
        *used = length - offset < capacity ? length - offset : capacity;
        memcpy(out, json + offset, *used); return ESP_OK;
    }
    if (kind == 4) {
        if (n != 0 && n != 2) return ESP_ERR_INVALID_ARG;
        char info[512];
        size_t length = ble_ota_info(info, sizeof(info));
        size_t offset = n ? get16(p) : 0;
        if (!length || offset > length) return ESP_ERR_INVALID_ARG;
        *used = length - offset < capacity ? length - offset : capacity;
        memcpy(out, info + offset, *used);
        return ESP_OK;
    }
    if (capacity < 20) return ESP_ERR_INVALID_SIZE;
    if (kind == 5) {
        if (owner != OTA_OWNER_USB) return ESP_ERR_NOT_ALLOWED;
        if (n != 2) return ESP_ERR_INVALID_ARG;
        err = ble_ota_arm_transport(get16(p), OTA_ALLOW_USB);
        if (err != ESP_OK) return err;
    } else if (kind == 1 || kind == 2) {
        if (!n) return ESP_ERR_INVALID_ARG;
        uint8_t op = kind == 2 ? 2 : p[0];
        err = ble_ota_submit(owner, op, p + (kind == 1), n - (kind == 1));
        if (err != ESP_OK) return err;
        /* Reply only after the shared worker has committed this operation.
         * Never block TinyUSB callbacks; USB requests execute on dap_worker. */
        int64_t deadline = esp_timer_get_time() + 15000000;
        for (;;) {
            ble_ota_status_for(owner, out);
            if (!(out[3] & 2)) break;
            if (esp_timer_get_time() >= deadline) {
                ble_ota_disconnected(owner);
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(1);
        }
    } else if (kind != 3 || n != 0) return ESP_ERR_INVALID_ARG;
    ble_ota_status_for(owner, out);
    *used = 20;
    return ESP_OK;
}

uint32_t wired_ota_usb(const uint8_t *req, size_t length, uint8_t *resp, size_t capacity)
{
    if (capacity < 7) return 0;
    resp[0] = 0x80;
    resp[1] = length >= 2 ? req[1] : 0;
    size_t used = 0;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (length >= 3 && req[2] <= length - 3)
        err = request(OTA_OWNER_USB, req[1], req + 3, req[2], resp + 7, capacity - 7, &used);
    resp[2] = used;
    put32(resp + 3, err);
    return 7 + used;
}

static bool receive_all(int fd, uint8_t *p, size_t n)
{
    while (n) {
        int count = recv(fd, p, n, 0);
        if (count <= 0) return false;
        p += count; n -= count;
    }
    return true;
}
static bool send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        int count = send(fd, p, n, 0);
        if (count <= 0) return false;
        p += count; n -= count;
    }
    return true;
}

static void tcp_task(void *arg)
{
    int server = (int)(intptr_t)arg;
    uint8_t input[OTA_MAX_PAYLOAD], output[516], header[8];
    ESP_LOGI(TAG, "WiFi self-OTA listening on TCP :%d", CONFIG_LINK_OTA_TCP_PORT);
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        struct timeval timeout = {.tv_sec = 10};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        while (receive_all(fd, header, sizeof(header))) {
            size_t n = get16(header + 6);
            if (memcmp(header, "WLO1", 4) || header[5] || n > sizeof(input) ||
                !receive_all(fd, input, n)) break;
            size_t used = 0;
            esp_err_t err = request(OTA_OWNER_WIFI, header[4], input, n,
                                    output + 4, sizeof(output) - 4, &used);
            put32(output, err);
            header[4] |= 0x80;
            header[6] = (used + 4) & 0xff;
            header[7] = (used + 4) >> 8;
            if (!send_all(fd, header, 8) || !send_all(fd, output, used + 4)) break;
        }
        ble_ota_disconnected(OTA_OWNER_WIFI);
        close(fd);
    }
}

esp_err_t wired_ota_net_start(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return ESP_FAIL;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(CONFIG_LINK_OTA_TCP_PORT),
                                  .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 1)) {
        close(fd); return ESP_FAIL;
    }
    if (xTaskCreate(tcp_task, "tcp_ota", 6144, (void *)(intptr_t)fd, 5, NULL) != pdPASS) {
        close(fd); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
