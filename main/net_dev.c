/**
 * @file  net_dev.c
 * @brief  WiFi STA 和可选热点，提供
 *           - 5555 端口：pyOCD remote 探针（一行一条 JSON）
 *           - 5556 端口：目标串口原始字节桥
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

#include "app_cfg.h"
#include "ble_dev.h"
#include "dap_server.h"
#include "link.h"
#include "net_dev.h"
#include "serial_bridge.h"
#include "usb_dev.h"

static const char *TAG = "net";

static char     s_sta_ip[16] = "0.0.0.0";
static char     s_ap_ip[16];
static char     s_ssid[33];
static bool     s_sta_connected;
static bool     s_sta_configured;
static unsigned s_ap_clients, s_sta_failures, s_sta_retry_seconds;
static volatile int s_dap_fd = -1;
static volatile uint32_t s_dap_epoch = 0;   /* 每次连接自增，防止 fd 复用时写错连接 */
static volatile int s_serial_fd = -1;
static uint32_t s_serial_epoch;
static SemaphoreHandle_t s_dap_send_lock, s_serial_send_lock;
static QueueHandle_t s_serial_tx_queue;

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

/* ---------------- 事件 ---------------- */

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    s_sta_retry_seconds = 0;
    if (s_sta_configured && !s_sta_connected && !s_ap_clients) esp_wifi_connect();
}

static esp_timer_handle_t s_reconnect_timer;

static void schedule_reconnect(void)
{
    if (!s_sta_configured || s_sta_connected || s_ap_clients) return;
    unsigned shift = s_sta_failures < 5 ? s_sta_failures++ : 5;
    unsigned seconds = 2u << shift;
    if (seconds > 60) seconds = 60;
    s_sta_retry_seconds = seconds;
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)seconds * 1000000);
    ESP_LOGI(TAG, "STA retry in %u seconds (no AP clients)", seconds);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) {
            if (id == WIFI_EVENT_STA_DISCONNECTED) {
                s_sta_connected = false;
                snprintf(s_sta_ip, sizeof(s_sta_ip), "0.0.0.0");
                schedule_reconnect();
            } else if (s_sta_configured && !s_ap_clients) {
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            ++s_ap_clients;
            esp_timer_stop(s_reconnect_timer);
            s_sta_retry_seconds = 0;
            /* A stale router configuration must not keep scanning channels
             * while a client is using the local OTA / DAP access point. */
            if (!s_sta_connected) (void)esp_wifi_disconnect();
            ESP_LOGI(TAG, "AP clients=%u; failed STA retries paused", s_ap_clients);
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            if (s_ap_clients) --s_ap_clients;
            if (!s_ap_clients) schedule_reconnect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        s_sta_failures = s_sta_retry_seconds = 0;
        ESP_LOGI(TAG, "STA 已获得 IP: %s", s_sta_ip);
        ESP_LOGI(TAG, "pyOCD 用法: pyocd ... --probe remote:%s:%d", s_sta_ip, cfg_dap_tcp_port());
    }
}

/* ---------------- TCP 发送 ---------------- */

static bool send_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, data + off, len - off, 0);
        if (n <= 0) {
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

bool tcp_dap_send(int opaque, const uint8_t *data, size_t len)
{
    if (!s_dap_send_lock || xSemaphoreTake(s_dap_send_lock, pdMS_TO_TICKS(1100)) != pdTRUE) return false;
    /* 低 16 位是 fd，高 16 位是会话代号：fd 被复用也不会写错连接 */
    const int fd = opaque & 0xFFFF;
    const uint32_t epoch = ((uint32_t)opaque >> 16) & 0x7FFFu;
    if (fd < 0 || fd != s_dap_fd || epoch != (s_dap_epoch & 0x7FFFu)) {
        xSemaphoreGive(s_dap_send_lock);
        return false;
    }
    /* 协议：一行一条 JSON，响应也要以 '\n' 结尾 */
    static const uint8_t nl = '\n';
    bool ok = send_all(fd, data, len) && send_all(fd, &nl, 1);
    if (!ok) shutdown(fd, SHUT_RDWR);
    xSemaphoreGive(s_dap_send_lock);
    return ok;
}

static void tcp_serial_tx_task(void *arg)
{
    (void)arg;
    serial_output_chunk_t chunk;
    for (;;) {
        if (xQueueReceive(s_serial_tx_queue, &chunk, portMAX_DELAY) != pdTRUE) continue;
        int fd = chunk.token & 0xffff;
        uint32_t epoch = (uint32_t)chunk.token >> 16;
        xSemaphoreTake(s_serial_send_lock, portMAX_DELAY);
        bool current = fd == s_serial_fd && epoch == (s_serial_epoch & 0x7fffu);
        size_t sent = 0;
        if (current) {
            while (sent < chunk.len) {
                int n = send(fd, chunk.data + sent, chunk.len - sent, 0);
                if (n <= 0) { shutdown(fd, SHUT_RDWR); break; }
                sent += (size_t)n;
            }
        }
        xSemaphoreGive(s_serial_send_lock);
        serial_bridge_note_drop(SERIAL_OUTPUT_TCP, chunk.len - sent);
    }
}

/* ---------------- DAP TCP 服务 ---------------- */

/* TCP probes reclaim unreachable peers in about 60 seconds while keeping
 * responsive but idle debug/serial clients connected. */
static bool tune_client_fd(int fd)
{
    const int one = 1, idle = 30, interval = 10, probes = 3;
    const struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0 &&
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) == 0 &&
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) == 0 &&
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) == 0;
}

static void tcp_dap_task(void *arg)
{
    (void)arg;
    const int port = cfg_dap_tcp_port();
    char *buf = malloc(DAP_JSON_BUF + 4096);
    if (buf == NULL) {
        ESP_LOGE(TAG, "分配接收缓冲失败");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (srv < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)port);
        if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 2) != 0) {
            ESP_LOGE(TAG, "DAP 端口 %d 绑定失败 (errno=%d)", port, errno);
            close(srv);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        ESP_LOGI(TAG, "DAP 服务监听 TCP :%d", port);

        for (;;) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(srv, &rfds);
            struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
            int ready = select(srv + 1, &rfds, NULL, NULL, &tv);
            if (ready <= 0) {
                continue;
            }
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int fd = accept(srv, (struct sockaddr *)&peer, &plen);
            if (fd < 0) {
                ESP_LOGW(TAG, "accept 失败 (errno=%d)", errno);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (!tune_client_fd(fd)) {
                ESP_LOGW(TAG, "DAP 客户端套接字配置失败 (errno=%d)", errno);
                close(fd);
                continue;
            }
            xSemaphoreTake(s_dap_send_lock, portMAX_DELAY);
            s_dap_epoch++;
            s_dap_fd = fd;
            int token = fd | (int)((s_dap_epoch & 0x7fffu) << 16);
            xSemaphoreGive(s_dap_send_lock);
            link_session_set(LINK_SINK_TCP, token);
            ESP_LOGI(TAG, "DAP 客户端 %s:%d 已连接", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

            size_t used = 0;
            for (;;) {
                if (used >= DAP_JSON_BUF) {
                    break;  /* Never execute the suffix of an oversized JSON line. */
                }
                int n = recv(fd, buf + used, DAP_JSON_BUF - used, 0);
                if (n <= 0) {
                    break;
                }
                used += (size_t)n;
                char *start = buf;
                char *end = buf + used;
                char *nl;
                while ((nl = memchr(start, '\n', (size_t)(end - start))) != NULL) {
                    size_t len = (size_t)(nl - start);
                    while (len > 0 && start[len - 1] == '\r') {
                        --len;
                    }
                    if (len > 0) {
                        if (link_submit(LINK_SINK_TCP, token, (const uint8_t *)start, len) != ESP_OK) {
                            /* Terminate instead of silently losing a DAP response. */
                            shutdown(fd, SHUT_RDWR);
                        }
                    }
                    start = nl + 1;
                }
                size_t remain = (size_t)(end - start);
                if (remain > 0 && start != buf) {
                    memmove(buf, start, remain);
                }
                used = remain;
            }
            ESP_LOGI(TAG, "DAP 客户端断开");
            link_session_set(LINK_SINK_TCP, -1);
            xSemaphoreTake(s_dap_send_lock, portMAX_DELAY);
            s_dap_fd = -1;
            close(fd);
            xSemaphoreGive(s_dap_send_lock);
        }
    }
}

/* ---------------- 串口 TCP 桥 ---------------- */

static bool serial_tcp_sink(int token, const uint8_t *data, size_t len)
{
    serial_output_chunk_t chunk = {.token = token, .len = len};
    memcpy(chunk.data, data, len);
    if (xQueueSend(s_serial_tx_queue, &chunk, 0) == pdTRUE) return true;
    serial_bridge_note_drop(SERIAL_OUTPUT_TCP, len);
    return false;
}

static bool queue_tcp_serial(int fd, const serial_input_chunk_t *chunk)
{
    for (;;) {
        esp_err_t err = serial_bridge_to_target(chunk, pdMS_TO_TICKS(250));
        if (err != ESP_ERR_TIMEOUT) return err == ESP_OK;

        /* A keepalive error must also release the socket while UART is full. */
        struct pollfd socket = {.fd = fd};
        int ready = poll(&socket, 1, 0);
        if (ready < 0 || (socket.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
    }
}

static void tcp_serial_task(void *arg)
{
    (void)arg;
    const int port = cfg_serial_tcp_port();
    serial_input_chunk_t chunk = {0};

    for (;;) {
        int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (srv < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)port);
        if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 1) != 0) {
            ESP_LOGE(TAG, "串口端口 %d 绑定失败 (errno=%d)", port, errno);
            close(srv);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        ESP_LOGI(TAG, "串口桥监听 TCP :%d", port);

        for (;;) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(srv, &rfds);
            struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
            int ready = select(srv + 1, &rfds, NULL, NULL, &tv);
            if (ready <= 0) {
                continue;
            }
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int fd = accept(srv, (struct sockaddr *)&peer, &plen);
            if (fd < 0) {
                ESP_LOGW(TAG, "accept 失败 (errno=%d)", errno);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (!tune_client_fd(fd)) {
                ESP_LOGW(TAG, "串口客户端套接字配置失败 (errno=%d)", errno);
                close(fd);
                continue;
            }
            xSemaphoreTake(s_serial_send_lock, portMAX_DELAY);
            s_serial_fd = fd;
            ++s_serial_epoch;
            int token = fd | (int)((s_serial_epoch & 0x7fffu) << 16);
            xSemaphoreGive(s_serial_send_lock);
            if (serial_bridge_subscribe(serial_tcp_sink, token) != ESP_OK) {
                xSemaphoreTake(s_serial_send_lock, portMAX_DELAY);
                s_serial_fd = -1;
                close(fd);
                xSemaphoreGive(s_serial_send_lock);
                continue;
            }
            ESP_LOGI(TAG, "串口客户端 %s:%d 已连接", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

            for (;;) {
                int n = recv(fd, chunk.data, sizeof(chunk.data), 0);
                if (n <= 0) {
                    break;
                }
                chunk.len = (size_t)n;
                if (!queue_tcp_serial(fd, &chunk)) break;
            }
            serial_bridge_unsubscribe(serial_tcp_sink, token);
            xSemaphoreTake(s_serial_send_lock, portMAX_DELAY);
            s_serial_fd = -1;
            close(fd);
            xSemaphoreGive(s_serial_send_lock);
            ESP_LOGI(TAG, "串口客户端断开");
        }
    }
}

/* ---------------- 初始化 ---------------- */

esp_err_t net_dev_init(void)
{
    s_dap_send_lock = xSemaphoreCreateMutex();
    s_serial_send_lock = xSemaphoreCreateMutex();
    if (!s_dap_send_lock || !s_serial_send_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    const size_t ap_password_len = strlen(CONFIG_LINK_AP_PASSWORD);
    const bool ap_enabled = ap_password_len != 0;
    if (ap_enabled && (ap_password_len < 8 || ap_password_len > 63 ||
                       strlen(CONFIG_LINK_AP_SSID) == 0 || strlen(CONFIG_LINK_AP_SSID) > 32)) {
        ESP_LOGE(TAG, "热点要求 1–32 字节 SSID 和 8–63 字节 WPA2 口令");
        return ESP_ERR_INVALID_ARG;
    }
    if (ap_enabled) s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, "esp-swd");

    link_register_sender(LINK_SINK_TCP, tcp_dap_send);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t targs = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    esp_timer_create(&targs, &s_reconnect_timer);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    err = esp_wifi_set_mode(ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    if (ap_enabled) {
        wifi_config_t ap_cfg = {0};
        ap_cfg.ap.ssid_len = (uint8_t)strlen(CONFIG_LINK_AP_SSID);
        memcpy(ap_cfg.ap.ssid, CONFIG_LINK_AP_SSID, ap_cfg.ap.ssid_len);
        memcpy(ap_cfg.ap.password, CONFIG_LINK_AP_PASSWORD, ap_password_len);
        ap_cfg.ap.channel = 6;
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (err != ESP_OK) return err;
    }

    /* STA */
    char ssid[33] = {0};
    char pass[65] = {0};
    if (cfg_wifi_get(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t sta_cfg = {0};
        memcpy(sta_cfg.sta.ssid, ssid, strnlen(ssid, sizeof(sta_cfg.sta.ssid)));
        memcpy(sta_cfg.sta.password, pass, strnlen(pass, sizeof(sta_cfg.sta.password)));
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WiFi STA 配置无效: %s", esp_err_to_name(err));
        } else {
            s_sta_configured = true;
            snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        }
    } else {
        ESP_LOGW(TAG, "未配置路由器 WiFi，可通过 USB 或 BLE 配网");
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_ip_info_t ip;
    if (ap_enabled && esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip.ip));
    }

    if (xTaskCreate(tcp_dap_task, "tcp_dap", 6144, NULL, 9, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_serial_tx_queue = xQueueCreate(4, sizeof(serial_output_chunk_t));
    if (!s_serial_tx_queue) return ESP_ERR_NO_MEM;
    TaskHandle_t sender;
    if (xTaskCreate(tcp_serial_tx_task, "tcp_serial_tx", 3072, NULL, 7, &sender) != pdPASS) {
        vQueueDelete(s_serial_tx_queue);
        s_serial_tx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(tcp_serial_task, "tcp_serial", 4096, NULL, 7, NULL) != pdPASS) {
        vTaskDelete(sender);
        vQueueDelete(s_serial_tx_queue);
        s_serial_tx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (ap_enabled) ESP_LOGI(TAG, "热点: %s，地址 %s", CONFIG_LINK_AP_SSID, s_ap_ip);
    return ESP_OK;
}

const char *net_dev_ip(void)
{
    return s_sta_connected || !s_ap_ip[0] ? s_sta_ip : s_ap_ip;
}

const char *net_dev_ap_ip(void)
{
    return s_ap_ip;
}

bool net_dev_sta_connected(void)
{
    return s_sta_connected;
}

const char *net_dev_ssid(void)
{
    return s_ssid[0] ? s_ssid : "(未配置)";
}

unsigned net_dev_ap_clients(void) { return s_ap_clients; }
unsigned net_dev_sta_retry_seconds(void) { return s_sta_retry_seconds; }
