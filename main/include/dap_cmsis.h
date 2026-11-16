/**
 * @file  dap_cmsis.h
 * @brief  CMSIS-DAP v1 命令层（USB HID 64 字节报文）。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAP_PACKET_SIZE 64

/**
 * 处理一条 CMSIS-DAP 请求报文。
 * @param req       请求数据
 * @param req_len   请求长度
 * @param resp      响应缓冲区（至少 DAP_PACKET_SIZE）
 * @param resp_size 响应缓冲区大小
 * @return 响应长度（>0）
 */
uint32_t dap_cmsis_process(const uint8_t *req, uint32_t req_len,
                           uint8_t *resp, uint32_t resp_size);

#ifdef __cplusplus
}
#endif
