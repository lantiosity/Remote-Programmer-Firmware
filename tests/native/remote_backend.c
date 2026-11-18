#include <string.h>
#include "esp_app_desc.h"
#include "probe_core.h"
#include "ota_policy.h"
#include "wired_ota.h"
#include "link.c"

bool test_ota_window(int64_t now, int64_t deadline, unsigned allowed, unsigned requested)
{ return ota_window_open(now, deadline, allowed, requested); }

unsigned test_count, test_apsel, test_calls, test_bits;
unsigned test_connects, test_resets, test_disconnects;
int64_t test_now;
bool test_send_failed;
uint8_t test_data[8192];
static dap_session_t test_parser_session;
const char *cfg_serial_number(void) { return "native-test"; }
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = {.version = "native-test"};
    return &app;
}
int64_t esp_timer_get_time(void) { return test_now; }
uint32_t wired_ota_usb(const uint8_t *req, size_t length, uint8_t *resp, size_t capacity)
{
    return 0;
}
esp_err_t probe_connect(void) { ++test_connects; return ESP_OK; }
esp_err_t probe_disconnect(void) { ++test_disconnects; return ESP_OK; }
probe_status_t probe_reset_target(void) { ++test_resets; return PROBE_OK; }
probe_status_t probe_mem_select_ap(uint32_t ap, uint32_t bank)
{ test_apsel = ap; return PROBE_OK; }
probe_status_t probe_mem_write_block8(uint32_t addr, const uint8_t *data, uint32_t count)
{ ++test_calls; test_count = count; memcpy(test_data, data, count); return PROBE_OK; }
probe_status_t probe_mem_read_block8(uint32_t addr, uint8_t *data, uint32_t count)
{ ++test_calls; memset(data, 255, count); return PROBE_OK; }
probe_status_t probe_mem_read_block32(uint32_t addr, uint32_t *data, uint32_t count)
{ ++test_calls; memset(data, 255, count * 4); return PROBE_OK; }
void probe_swj_sequence(uint32_t count, const uint8_t *data)
{ ++test_calls; test_bits = count; memset(test_data, 0, sizeof(test_data)); memcpy(test_data, data, (count + 7) / 8); }

size_t test_remote_process(const char *line, size_t length, char *out, size_t capacity)
{
    dap_client_t client = {.session = &test_parser_session, .sink = LINK_SINK_TCP, .token = 1};
    return dap_server_process(line, length, out, capacity, &client);
}

static uint8_t test_response[DAP_JSON_BUF];
static size_t test_response_length;

static bool capture_response(int token, const uint8_t *data, size_t length)
{
    test_response_length = length;
    memcpy(test_response, data, length);
    return !test_send_failed;
}

void test_link_init(void)
{
    release_swd(true);
    dap_server_select_session(0);
    dap_server_reset_handles();
    dap_server_select_session(1);
    dap_server_reset_handles();
    for (link_sink_t sink = LINK_SINK_USB; sink < LINK_SINK_COUNT; ++sink) {
        link_session_set(sink, 1);
        link_register_sender(sink, capture_response);
    }
    test_now = 1;
    test_connects = test_resets = test_disconnects = 0;
    test_send_failed = false;
}

void test_link_poll(void) { process_request(NULL); }

size_t test_link_request(link_sink_t sink, int token, const uint8_t *data, size_t length, uint8_t *out)
{
    link_req_t *req = malloc(sizeof(*req) + length);
    if (!req) return 0;
    *req = (link_req_t){.sink = sink, .opaque = token, .len = length};
    memcpy(req->data, data, length);
    test_response_length = 0;
    process_request(req);
    free(req);
    memcpy(out, test_response, test_response_length);
    return test_response_length;
}
