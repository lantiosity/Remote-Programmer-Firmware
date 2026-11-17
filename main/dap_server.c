/**
 * @file  dap_server.c
 * @brief  pyOCD remote 探针协议（JSON/行）实现。
 *
 * pyOCD >= 0.36 内置 "remote" 探针插件，用法：
 *     pyocd flash --probe remote:192.168.4.1:5555 -t stm32f103rc app.hex
 * 协议为一行 JSON 请求 / 一行 JSON 响应，是 pyOCD 自带 tcp_probe_server 的对端实现。
 */
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "app_cfg.h"
#include "dap_server.h"
#include "probe_core.h"

static const char *TAG = "dap-remote";

/* pyOCD 的状态码 */
#define STATUS_OK           0
#define STATUS_GENERAL      1
#define STATUS_DISCONNECTED 2
#define STATUS_PROBE_ERR    3
#define STATUS_XFER_ERR     10
#define STATUS_XFER_TIMEOUT 11
#define STATUS_XFER_FAULT   12

#define MAX_AP_HANDLES  4
#define MAX_BLOCK_WORDS 2048
/* 单条 JSON 文本长度上限（响应缓冲扣掉头部），用于反推安全的数据量 */
#define MAX_BLOCK32_WORDS DAP_MAX_READ_WORDS
#define MAX_BLOCK8_BYTES  DAP_MAX_READ_BYTES

static uint32_t s_block[MAX_BLOCK_WORDS];
static uint8_t  s_bytes[MAX_BLOCK_WORDS * 4];

typedef struct {
    bool     used;
    uint32_t apsel;
} ap_handle_t;

static ap_handle_t s_sessions[2][MAX_AP_HANDLES];
static ap_handle_t *s_handles = s_sessions[0];

/* ---------------- 字符串构建 ---------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ovf;
} sb_t;

static void sb_init(sb_t *sb, char *buf, size_t cap)
{
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->ovf = false;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void sb_put(sb_t *sb, const char *s)
{
    size_t n = strlen(s);
    if (sb->len + n + 1 > sb->cap) {
        n = (sb->cap > sb->len + 1) ? (sb->cap - sb->len - 1) : 0;
        sb->ovf = true;
    }
    if (n > 0) {
        memcpy(sb->buf + sb->len, s, n);
        sb->len += n;
        sb->buf[sb->len] = '\0';
    }
}

static void sb_printf(sb_t *sb, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_put(sb, tmp);
    }
}

static void resp_head(sb_t *sb, int id)
{
    sb_printf(sb, "{\"id\":%d,\"status\":0", id);
}

static void resp_ok(sb_t *sb, int id)
{
    resp_head(sb, id);
    sb_put(sb, "}");
}

static void resp_int(sb_t *sb, int id, uint32_t v)
{
    resp_head(sb, id);
    sb_printf(sb, ",\"result\":%" PRIu32 "}", v);
}

static void resp_bool(sb_t *sb, int id, bool v)
{
    resp_head(sb, id);
    sb_printf(sb, ",\"result\":%s}", v ? "true" : "false");
}

static void resp_str(sb_t *sb, int id, const char *s)
{
    resp_head(sb, id);
    sb_printf(sb, ",\"result\":\"%s\"}", s);
}

static void resp_null(sb_t *sb, int id)
{
    resp_head(sb, id);
    sb_put(sb, ",\"result\":null}");
}

static void resp_err(sb_t *sb, int id, int status, const char *msg)
{
    sb_printf(sb, "{\"id\":%d,\"status\":%d,\"error\":\"%s\"}", id, status, msg);
}

static int status_of(probe_status_t st)
{
    switch (st) {
    case PROBE_OK:
        return STATUS_OK;
    case PROBE_ERR_WAIT:
        return STATUS_XFER_TIMEOUT;
    case PROBE_ERR_FAULT:
        return STATUS_XFER_FAULT;
    case PROBE_ERR_PARITY:
        return STATUS_XFER_ERR;
    case PROBE_ERR_ARG:
        return STATUS_GENERAL;
    default:
        return STATUS_XFER_ERR;
    }
}

/* ---------------- 参数解析 ---------------- */

static bool arg_int(cJSON *args, int idx, int64_t *out)
{
    cJSON *it = cJSON_GetArrayItem(args, idx);
    if (it == NULL || !cJSON_IsNumber(it)) {
        return false;
    }
    double value = it->valuedouble;
    /* All numeric protocol arguments except SWJ bits are unsigned 32-bit
     * values (addresses, data, handles, counts). Reject instead of wrapping. */
    if (!isfinite(value) || value < 0 || value > UINT32_MAX || trunc(value) != value) return false;
    *out = (int64_t)value;
    return true;
}

static bool arg_bool(cJSON *args, int idx, bool *out)
{
    cJSON *it = cJSON_GetArrayItem(args, idx);
    if (it == NULL) {
        return false;
    }
    if (cJSON_IsBool(it)) {
        *out = cJSON_IsTrue(it);
        return true;
    }
    if (cJSON_IsNumber(it)) {
        *out = it->valueint != 0;
        return true;
    }
    return false;
}

static const char *arg_str(cJSON *args, int idx)
{
    cJSON *it = cJSON_GetArrayItem(args, idx);
    if (it == NULL || !cJSON_IsString(it)) {
        return NULL;
    }
    return it->valuestring;
}

/* ---------------- AP 选择 ---------------- */

static probe_status_t select_ap(uint32_t apsel)
{
    return probe_mem_select_ap(apsel & 0xFFu, 0);
}

/* addr = APSEL<<24 | APBANKSEL<<4 | A[3:2]，AP IDR 在 0xFC（bank 0xF） */
static probe_status_t select_ap_for_addr(uint32_t addr)
{
    return probe_mem_select_ap((addr >> 24) & 0xFFu, addr & 0xF0u);
}

static ap_handle_t *find_handle(int handle)
{
    if (handle < 0 || handle >= MAX_AP_HANDLES || !s_handles[handle].used) {
        return NULL;
    }
    return &s_handles[handle];
}

/* ---------------- 请求处理 ---------------- */

static size_t process_property(sb_t *sb, int id, const char *name)
{
    if (strcmp(name, "unique_id") == 0) {
        resp_str(sb, id, cfg_serial_number());
    } else if (strcmp(name, "vendor_name") == 0) {
        resp_str(sb, id, "ESP-SWD");
    } else if (strcmp(name, "product_name") == 0) {
        resp_str(sb, id, "ESP32-S3 DAPLink");
    } else if (strcmp(name, "description") == 0) {
        resp_str(sb, id, "ESP32-S3 wireless CMSIS-DAP (USB/WiFi/BLE)");
    } else if (strcmp(name, "supported_wire_protocols") == 0) {
        resp_head(sb, id);
        sb_put(sb, ",\"result\":[\"SWD\",\"DEFAULT\"]}");
    } else if (strcmp(name, "wire_protocol") == 0) {
        if (probe_is_connected()) {
            resp_str(sb, id, "SWD");
        } else {
            resp_null(sb, id);
        }
    } else if (strcmp(name, "capabilities") == 0) {
        resp_head(sb, id);
        sb_put(sb, ",\"result\":[\"SWJ_SEQUENCE\"]}");
    } else {
        resp_err(sb, id, STATUS_GENERAL, "unknown property");
    }
    return sb->len;
}

static size_t build_array_u32(sb_t *sb, int id, const uint32_t *vals, uint32_t count)
{
    resp_head(sb, id);
    sb_put(sb, ",\"result\":[");
    for (uint32_t i = 0; i < count; ++i) {
        if (i) {
            sb_put(sb, ",");
        }
        sb_printf(sb, "%" PRIu32, vals[i]);
    }
    sb_put(sb, "]}");
    return sb->len;
}

static int parse_u32_array(cJSON *args, int idx, uint32_t *out, uint32_t max)
{
    cJSON *arr = cJSON_GetArrayItem(args, idx);
    if (arr == NULL || !cJSON_IsArray(arr)) {
        return -1;
    }
    uint32_t n = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (n >= max || !cJSON_IsNumber(it) || !isfinite(it->valuedouble) ||
            it->valuedouble < 0 || it->valuedouble > UINT32_MAX ||
            trunc(it->valuedouble) != it->valuedouble) return -1;
        out[n++] = (uint32_t)it->valuedouble;
    }
    return (int)n;
}

static int parse_byte_array(cJSON *args, int idx, uint8_t *out, size_t max)
{
    cJSON *arr = cJSON_GetArrayItem(args, idx), *it;
    if (!cJSON_IsArray(arr)) return -1;
    size_t n = 0;
    cJSON_ArrayForEach(it, arr) {
        if (n >= max || !cJSON_IsNumber(it) || !isfinite(it->valuedouble) ||
            it->valuedouble < 0 || it->valuedouble > 255 ||
            trunc(it->valuedouble) != it->valuedouble) return -1;
        out[n++] = (uint8_t)it->valuedouble;
    }
    return (int)n;
}

/* cJSON stores numbers as doubles. Recover the second arguments token from
 * the validated JSON text, skipping strings/nested values, before rounding. */
static const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')) ++p;
    return p;
}

static const char *skip_json_value(const char *p, const char *end)
{
    int depth = 0;
    bool quoted = false;
    for (; p < end; ++p) {
        if (quoted) {
            if (*p == '\\' && p + 1 < end) ++p;
            else if (*p == '"') quoted = false;
        } else if (*p == '"') quoted = true;
        else if (*p == '[' || *p == '{') ++depth;
        else if (*p == ']' || *p == '}') {
            if (!depth) break;
            if (--depth == 0) return p + 1;
        } else if (*p == ',' && !depth) break;
    }
    return p;
}

static bool parse_swj_bits(const char *line, size_t length, cJSON *args, uint8_t out[32])
{
    const char *begin = NULL, *end = NULL;
    cJSON *item = cJSON_GetArrayItem(args, 1);
    unsigned base = 10;
    if (cJSON_IsString(item)) {
        begin = item->valuestring;
        end = begin + strlen(begin);
        if (end - begin > 2 && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X')) {
            base = 16;
            begin += 2;
        }
    } else if (cJSON_IsNumber(item)) {
        const char *limit = line + length;
        const char *p = skip_space(line, limit);
        if (p == limit || *p++ != '{') return false;
        while ((p = skip_space(p, limit)) < limit && *p == '"') {
            const char *key = p++;
            while (p < limit && *p != '"') { if (*p == '\\') ++p; ++p; }
            if (p == limit) return false;
            size_t key_len = (size_t)(p + 1 - key);
            p = skip_space(p + 1, limit);
            if (p == limit || *p++ != ':') return false;
            p = skip_space(p, limit);
            if (key_len == 11 && !memcmp(key, "\"arguments\"", 11)) {
                if (p == limit || *p++ != '[') return false;
                p = skip_space(skip_json_value(p, limit), limit);
                if (p == limit || *p++ != ',') return false;
                begin = skip_space(p, limit);
                end = begin;
                while (end < limit && *end >= '0' && *end <= '9') ++end;
                p = skip_space(end, limit);
                if (p == limit || (*p != ',' && *p != ']')) return false;
                break;
            }
            p = skip_space(skip_json_value(p, limit), limit);
            if (p == limit || *p++ != ',') break;
        }
    }
    if (!begin || !end || begin == end) return false;
    memset(out, 0, 32);
    for (const char *p = begin; p < end; ++p) {
        unsigned digit = *p >= '0' && *p <= '9' ? (unsigned)(*p - '0') :
            *p >= 'a' && *p <= 'f' ? (unsigned)(*p - 'a' + 10) :
            *p >= 'A' && *p <= 'F' ? (unsigned)(*p - 'A' + 10) : 255;
        if (digit >= base) return false;
        unsigned carry = digit;
        for (unsigned j = 0; j < 32; ++j) {
            carry += out[j] * base;
            out[j] = (uint8_t)carry;
            carry >>= 8;
        }
        if (carry) return false;
    }
    return true;
}

size_t dap_server_process(const char *line, size_t line_len, char *out, size_t out_size)
{
    sb_t sb;
    sb_init(&sb, out, out_size);

    if (!line || !line_len || line_len >= DAP_JSON_BUF) {
        resp_err(&sb, -1, STATUS_GENERAL, "invalid request size");
        return sb.len;
    }
    const char *parsed_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(line, line_len, &parsed_end, false);
    if (root == NULL || !cJSON_IsObject(root) || skip_space(parsed_end, line + line_len) != line + line_len) {
        resp_err(&sb, -1, STATUS_GENERAL, "invalid json");
        cJSON_Delete(root);
        return sb.len;
    }

    int id = -1;
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(jid)) {
        id = jid->valueint;
    }
    cJSON *jreq = cJSON_GetObjectItemCaseSensitive(root, "request");
    cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "arguments");

    if (!cJSON_IsString(jreq)) {
        resp_err(&sb, id, STATUS_GENERAL, "missing request field");
        cJSON_Delete(root);
        return sb.len;
    }

    const char *name = jreq->valuestring;
    int64_t a = 0, b = 0, c = 0, d = 0;

    if (strcmp(name, "hello") == 0) {
        if (!arg_int(args, 0, &a) || a != 1) {
            resp_err(&sb, id, STATUS_GENERAL, "unsupported protocol version");
        } else {
            resp_ok(&sb, id);
        }
    } else if (strcmp(name, "readprop") == 0) {
        const char *prop = arg_str(args, 0);
        if (prop == NULL) {
            resp_err(&sb, id, STATUS_GENERAL, "missing property name");
        } else {
            process_property(&sb, id, prop);
        }
    } else if (strcmp(name, "open") == 0) {
        probe_connect();
        resp_ok(&sb, id);
    } else if (strcmp(name, "close") == 0) {
        dap_server_reset_session();
        resp_ok(&sb, id);
    } else if (strcmp(name, "lock") == 0 || strcmp(name, "unlock") == 0) {
        resp_ok(&sb, id);
    } else if (strcmp(name, "connect") == 0) {
        const char *proto = arg_str(args, 0);
        if (proto == NULL) {
            proto = "SWD";
        }
        if (strcmp(proto, "SWD") != 0 && strcmp(proto, "DEFAULT") != 0) {
            resp_err(&sb, id, STATUS_PROBE_ERR, "only SWD is supported");
        } else {
            probe_connect();
            resp_ok(&sb, id);
        }
    } else if (strcmp(name, "disconnect") == 0) {
        probe_disconnect();
        resp_ok(&sb, id);
    } else if (strcmp(name, "set_clock") == 0) {
        if (arg_int(args, 0, &a)) {
            probe_set_clock((uint32_t)a);
            resp_ok(&sb, id);
        } else {
            resp_err(&sb, id, STATUS_GENERAL, "missing frequency");
        }
    } else if (strcmp(name, "swj_sequence") == 0) {
        int64_t count = 0;
        uint8_t buf[32];
        if (!arg_int(args, 0, &count) || count < 1 || count > 256 ||
            !parse_swj_bits(line, line_len, args, buf)) {
            resp_err(&sb, id, STATUS_GENERAL, "bad swj_sequence arguments");
        } else {
            uint32_t bit_count = (uint32_t)count;
            probe_swj_sequence(bit_count, buf);
            ESP_LOGD(TAG, "swj_sequence %u bits", (unsigned)bit_count);
            resp_ok(&sb, id);
        }
    } else if (strcmp(name, "reset") == 0) {
        probe_status_t st = probe_reset_target();
        if (st == PROBE_OK) {
            resp_ok(&sb, id);
        } else {
            resp_err(&sb, id, status_of(st), "reset failed");
        }
    } else if (strcmp(name, "assert_reset") == 0) {
        bool asserted = false;
        if (arg_bool(args, 0, &asserted)) {
            probe_assert_reset(asserted);
            resp_ok(&sb, id);
        } else {
            resp_err(&sb, id, STATUS_GENERAL, "missing argument");
        }
    } else if (strcmp(name, "is_reset_asserted") == 0) {
        resp_bool(&sb, id, probe_is_reset_asserted());
    } else if (strcmp(name, "flush") == 0) {
        (void)probe_mem_flush();
        resp_ok(&sb, id);
    } else if (strcmp(name, "read_dp") == 0) {
        if (arg_int(args, 0, &a)) {
            uint32_t v = 0;
            probe_status_t st = probe_dp_read((uint32_t)a, &v);
            if (st == PROBE_OK) {
                resp_int(&sb, id, v);
            } else {
                resp_err(&sb, id, status_of(st), "DP read failed");
            }
        } else {
            resp_err(&sb, id, STATUS_GENERAL, "missing address");
        }
    } else if (strcmp(name, "write_dp") == 0) {
        if (arg_int(args, 0, &a) && arg_int(args, 1, &b)) {
            probe_status_t st = probe_dp_write((uint32_t)a, (uint32_t)b);
            if (st == PROBE_OK) {
                resp_ok(&sb, id);
            } else {
                resp_err(&sb, id, status_of(st), "DP write failed");
            }
        } else {
            resp_err(&sb, id, STATUS_GENERAL, "bad arguments");
        }
    } else if (strcmp(name, "read_ap") == 0) {
        if (arg_int(args, 0, &a)) {
            uint32_t addr = (uint32_t)a;
            probe_status_t st = select_ap_for_addr(addr);
            uint32_t v = 0;
            if (st == PROBE_OK) {
                st = probe_ap_read(addr & 0xFCu, &v);
            }
            if (st == PROBE_OK) {
                resp_int(&sb, id, v);
            } else {
                resp_err(&sb, id, status_of(st), "AP read failed");
            }
        } else {
            resp_err(&sb, id, STATUS_GENERAL, "missing address");
        }
    } else if (strcmp(name, "write_ap") == 0) {
        if (arg_int(args, 0, &a) && arg_int(args, 1, &b)) {
            uint32_t addr = (uint32_t)a;
            probe_status_t st = select_ap_for_addr(addr);
            if (st == PROBE_OK) {
                st = probe_ap_write(addr & 0xFCu, (uint32_t)b);
            }
            if (st == PROBE_OK) {
                resp_ok(&sb, id);
            } else {
                resp_err(&sb, id, status_of(st), "AP write failed");
            }
        } else {
            resp_err(&sb, id, STATUS_GENERAL, "bad arguments");
        }
    } else if (strcmp(name, "read_ap_multiple") == 0) {
        if (arg_int(args, 0, &a) && arg_int(args, 1, &b) && b > 0 && b <= MAX_BLOCK32_WORDS) {
            uint32_t addr = (uint32_t)a;
            probe_status_t st = select_ap_for_addr(addr);
            uint32_t n = (uint32_t)b;
            if (st == PROBE_OK) {
                st = probe_ap_read_multiple(addr & 0xFCu, s_block, n);
            }
            if (st == PROBE_OK) {
                build_array_u32(&sb, id, s_block, n);
            } else {
                resp_err(&sb, id, status_of(st), "AP block read failed");
            }
        } else {
            resp_err(&sb, id, STATUS_GENERAL, "bad arguments");
        }
    } else if (strcmp(name, "write_ap_multiple") == 0) {
        if (arg_int(args, 0, &a)) {
            int n = parse_u32_array(args, 1, s_block, MAX_BLOCK_WORDS);
            if (n <= 0) {
                resp_err(&sb, id, STATUS_GENERAL, "bad data array");
            } else {
                uint32_t addr = (uint32_t)a;
                probe_status_t st = select_ap_for_addr(addr);
                if (st == PROBE_OK) {
                    st = probe_ap_write_multiple(addr & 0xFCu, s_block, (uint32_t)n);
                }
                if (st == PROBE_OK) {
                    resp_ok(&sb, id);
                } else {
                    resp_err(&sb, id, status_of(st), "AP block write failed");
                }
            }
        } else {
            resp_err(&sb, id, STATUS_GENERAL, "bad arguments");
        }
    } else if (strcmp(name, "get_memory_interface_for_ap") == 0) {
        if (arg_int(args, 0, &a) && arg_int(args, 1, &b)) {
            int64_t version = a;
            uint32_t nominal = (uint32_t)b;
            if (version != 1 || b < 0 || b > 255) {
                resp_err(&sb, id, STATUS_GENERAL, "only APv1 APSEL 0..255 is supported");
            } else {
                uint32_t apsel = nominal;
                int handle = -1;
                for (int i = 0; i < MAX_AP_HANDLES; ++i) {
                    if (s_handles[i].used && s_handles[i].apsel == apsel) { handle = i; break; }
                }
                for (int i = 0; i < MAX_AP_HANDLES; ++i) {
                    if (handle < 0 && !s_handles[i].used) {
                        s_handles[i].used = true;
                        s_handles[i].apsel = apsel;
                        handle = i;
                        break;
                    }
                }
                if (handle < 0) {
                    resp_err(&sb, id, STATUS_GENERAL, "no free AP handle");
                } else {
                    resp_int(&sb, id, (uint32_t)handle);
                }
            }
        } else {
            resp_err(&sb, id, STATUS_GENERAL, "bad arguments");
        }
    } else if (strcmp(name, "read_mem") == 0) {
        ap_handle_t *h = NULL;
        if (arg_int(args, 0, &a)) {
            h = find_handle((int)a);
        }
        if (h == NULL || !arg_int(args, 1, &b) || !arg_int(args, 2, &c)) {
            resp_err(&sb, id, STATUS_GENERAL, "bad read_mem arguments");
        } else {
            probe_status_t st = select_ap(h->apsel);
            uint32_t v = 0;
            if (st == PROBE_OK) {
                st = probe_mem_read((uint32_t)b, (uint32_t)c, &v);
            }
            if (st == PROBE_OK) {
                resp_int(&sb, id, v);
            } else {
                resp_err(&sb, id, status_of(st), "read_mem failed");
            }
        }
    } else if (strcmp(name, "write_mem") == 0) {
        ap_handle_t *h = NULL;
        if (arg_int(args, 0, &a)) {
            h = find_handle((int)a);
        }
        if (h == NULL || !arg_int(args, 1, &b) || !arg_int(args, 2, &c) || !arg_int(args, 3, &d)) {
            resp_err(&sb, id, STATUS_GENERAL, "bad write_mem arguments");
        } else {
            probe_status_t st = select_ap(h->apsel);
            if (st == PROBE_OK) {
                st = probe_mem_write((uint32_t)b, (uint32_t)d, (uint32_t)c);
            }
            if (st == PROBE_OK) {
                resp_ok(&sb, id);
            } else {
                resp_err(&sb, id, status_of(st), "write_mem failed");
            }
        }
    } else if (strcmp(name, "read_block32") == 0) {
        ap_handle_t *h = NULL;
        if (arg_int(args, 0, &a)) {
            h = find_handle((int)a);
        }
        if (h == NULL || !arg_int(args, 1, &b) || !arg_int(args, 2, &c) ||
            c <= 0 || c > MAX_BLOCK32_WORDS) {
            resp_err(&sb, id, STATUS_GENERAL, "bad read_block32 arguments");
        } else {
            probe_status_t st = select_ap(h->apsel);
            uint32_t n = (uint32_t)c;
            if (st == PROBE_OK) {
                st = probe_mem_read_block32((uint32_t)b, s_block, n);
            }
            if (st == PROBE_OK) {
                build_array_u32(&sb, id, s_block, n);
            } else {
                resp_err(&sb, id, status_of(st), "read_block32 failed");
            }
        }
    } else if (strcmp(name, "write_block32") == 0) {
        ap_handle_t *h = NULL;
        if (arg_int(args, 0, &a)) {
            h = find_handle((int)a);
        }
        int n = -1;
        if (arg_int(args, 1, &b)) {
            n = parse_u32_array(args, 2, s_block, MAX_BLOCK_WORDS);
        }
        if (h == NULL || n <= 0) {
            resp_err(&sb, id, STATUS_GENERAL, "bad write_block32 arguments");
        } else {
            probe_status_t st = select_ap(h->apsel);
            if (st == PROBE_OK) {
                st = probe_mem_write_block32((uint32_t)b, s_block, (uint32_t)n);
            }
            if (st == PROBE_OK) {
                resp_ok(&sb, id);
            } else {
                resp_err(&sb, id, status_of(st), "write_block32 failed");
            }
        }
    } else if (strcmp(name, "read_block8") == 0) {
        ap_handle_t *h = NULL;
        if (arg_int(args, 0, &a)) {
            h = find_handle((int)a);
        }
        if (h == NULL || !arg_int(args, 1, &b) || !arg_int(args, 2, &c) ||
            c <= 0 || c > (int64_t)MAX_BLOCK8_BYTES) {
            resp_err(&sb, id, STATUS_GENERAL, "bad read_block8 arguments");
        } else {
            probe_status_t st = select_ap(h->apsel);
            uint32_t n = (uint32_t)c;
            if (st == PROBE_OK) {
                st = probe_mem_read_block8((uint32_t)b, s_bytes, n);
            }
            if (st == PROBE_OK) {
                resp_head(&sb, id);
                sb_put(&sb, ",\"result\":[");
                for (uint32_t i = 0; i < n; ++i) {
                    if (i) {
                        sb_put(&sb, ",");
                    }
                    sb_printf(&sb, "%u", s_bytes[i]);
                }
                sb_put(&sb, "]}");
            } else {
                resp_err(&sb, id, status_of(st), "read_block8 failed");
            }
        }
    } else if (strcmp(name, "write_block8") == 0) {
        ap_handle_t *h = NULL;
        if (arg_int(args, 0, &a)) {
            h = find_handle((int)a);
        }
        int n = -1;
        if (arg_int(args, 1, &b)) {
            n = parse_byte_array(args, 2, s_bytes, MAX_BLOCK8_BYTES);
        }
        if (h == NULL || n <= 0) {
            resp_err(&sb, id, STATUS_GENERAL, "bad write_block8 arguments");
        } else {
            probe_status_t st = select_ap(h->apsel);
            if (st == PROBE_OK) {
                st = probe_mem_write_block8((uint32_t)b, s_bytes, (uint32_t)n);
            }
            if (st == PROBE_OK) {
                resp_ok(&sb, id);
            } else {
                resp_err(&sb, id, status_of(st), "write_block8 failed");
            }
        }
    } else if (strcmp(name, "has_swo") == 0) {
        resp_bool(&sb, id, false);
    } else if (strcmp(name, "swo_start") == 0 || strcmp(name, "swo_stop") == 0 ||
               strcmp(name, "swo_read") == 0) {
        resp_err(&sb, id, STATUS_PROBE_ERR, "SWO not supported");
    } else if (strcmp(name, "jtag_sequence") == 0) {
        resp_err(&sb, id, STATUS_PROBE_ERR, "JTAG not supported");
    } else {
        ESP_LOGW(TAG, "unknown request '%s'", name);
        resp_err(&sb, id, STATUS_GENERAL, "unknown request type");
    }

    cJSON_Delete(root);

    /* 响应放不下时回一个明确的错误，绝不返回半截 JSON */
    if (sb.ovf) {
        sb_init(&sb, out, out_size);
        resp_err(&sb, id, STATUS_GENERAL, "response too large");
    }
    return sb.len;
}

void dap_server_reset_session(void)
{
    dap_server_reset_handles();
    probe_disconnect();
}

void dap_server_select_session(unsigned session)
{
    if (session < 2) s_handles = s_sessions[session];
}

void dap_server_reset_handles(void)
{
    memset(s_handles, 0, sizeof(s_sessions[0]));
}
