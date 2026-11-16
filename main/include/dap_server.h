/**
 * @file  dap_server.h
 * @brief  pyOCD remote 探针协议（JSON/行）服务端实现。
 *
 * 协议（pyOCD >= 0.36 内置 "remote" 探针插件）：
 *   请求: {"id":<int>,"request":<str>,"arguments":[..]}\n
 *   响应: {"id":<int>,"status":<int>,["error":<str>,]["result":<value>]}\n
 * 同一份实现在 WiFi(TCP) 与 BLE 上复用，因此 PC 端用 `--probe remote:<ip>:5555`
 * 即可，无需任何中间代理。
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 建议的响应缓冲大小（read_block32 的 JSON 文本可能很大） */
#define DAP_JSON_BUF 20480

/**
 * 处理一条请求（不含结尾换行）。
 * @param line       请求文本（NUL 结尾更安全）
 * @param line_len   文本长度
 * @param out        响应缓冲
 * @param out_size   响应缓冲大小
 * @return 响应文本长度（>0），不含结尾换行；0 表示无响应
 */
size_t dap_server_process(const char *line, size_t line_len, char *out, size_t out_size);

/** 主动断开当前"远端探针"会话状态（例如链路断开时） */
void dap_server_reset_session(void);

#ifdef __cplusplus
}
#endif
