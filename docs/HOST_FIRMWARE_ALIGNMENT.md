# 上位机 0.2.1 / 固件 0.1.1 对齐核对

核对日期：2026-11-18。上位机提交 `1e90b03`，固件发布提交 `21c10f5`（v0.1.1）。本次不改上位机或固件实现，不覆盖已有发布包。

结论：默认端口和常用请求路径在代码层兼容；尚不能认定所有边界及首次 BLE OTA 体验完全对齐，也没有完成跨设备硬件联调。

## 已匹配

| 项目 | 核对结果 |
| --- | --- |
| USB | VID/PID 0D28:0204，64 字节 HID，Windows 额外 report ID 0；OTA vendor 0x80、kind/长度/错误码位置一致。 |
| OTA | protocol=1、esp_swdlink 项目名、20 字节状态、BEGIN/DATA/FINISH/ACTIVATE/ABORT 操作码、SHA256 和 ELF 哈希偏移一致。USB 数据 56 字节、WiFi 1024 字节、BLE 至多 480 字节均在固件限制内。 |
| WiFi | DAP 5555、UART 5556、OTA 5557 的默认值一致；WLO1 帧及小端长度/错误码一致。 |
| 重启确认 | 上位机等待运行槽变化、boot=running、image_state=2 和 ELF 哈希匹配，与固件约 5 秒健康确认相符。 |
| Remote | pyOCD 0.46.1 protocol=1、状态码、APv1 nominal APSEL、内存参数顺序、SWJ 整数均匹配；APv2 明确不支持。 |
| BLE DAP | 上位机先订阅 TX，再带响应写 RX；固件异步通知按换行结束，上位机按字节流转发。 |
| UART/配网 | 上位机 DTR=false，固件 CDC 不强制 DTR；wifi_setup=1 能力识别兼容 0.1.1，HEX/请求 id/分页格式一致。 |
| 时钟 | 上位机允许请求 10～50000 kHz，物理层支持该请求范围；请求值不等于实际 SWCLK 测量值。 |

## 仍需对齐的边界

1. **P1，大块 remote 请求无自动拆分。** 发布引擎 `pyocd/probe/tcp_client_probe.py` 的 RemoteMemoryInterface 直接转发调用长度；固件读上限为 1856 words / 5104 bytes，整条请求小于 20480 字节。2048-word 读必然被拒绝；最坏十进制 word 数组也可能先超过行长。4096-byte 页写匹配且回归通过，但不能据此保证任意大小页、算法和内存读都兼容。后续应实现客户端分块或显式能力/长度协商；仅提高有限缓冲仍不能覆盖无界请求。
2. **P1，BLE OTA 首次加密/配对流程未在上位机显式处理。** 固件默认 WRITE_ENC；BleConnection 直接 GATT 写，没有 PairAsync 或 ProtectionLevel 配置，上位机升级页也没有配对提示。已配对连接可按现有协议使用；未配对时是否由 Windows 自动触发加密需实机验证，不能保证开箱成功。当前可先通过系统完成配对。
3. **P2，授权状态仍是全局提示。** 固件已按 transport 检查 BEGIN，但 OTA status 的 Flags bit0 及配网 status 的 armed 仍只表示“存在窗口”；上位机据此判断无线已授权。仅 USB 授权后换到 BLE/WiFi，界面预检查可能通过，真正 BEGIN/save 被拒绝；不会绕过固件权限。可先物理长按 BOOT，或 `ota arm 300 wifi/ble` 授权对应链路。后续应在信息接口暴露授权 mask，并让客户端按链路判断。
4. **P2，自定义 OTA 端口未打通。** 固件 LINK_OTA_TCP_PORT 可配置、info 报告实际值，但上位机 WiredOtaTransport 固定连接 5557，不读取/配置独立 OTA 端口。当前发布固件默认 5557 可用；自定义构建改端口后不可直接配套。

## 验证范围

- 现有上位机 C# 测试 **32/32** 通过；三项回环 socket 测试在沙箱内遇到 10013，受限环境外重跑全部通过，非已确认产品缺陷。
- 固件原生回归 **15/15** 通过，包括 4096-byte 写入、APSEL/SWJ、响应极值、TAR 回绕和订阅锁平衡。
- 本次测试均未访问物理 ESP32/STM32。独立单元测试通过不代表完整端到端验收；待验证 BLE 首次配对、真实 USB/WiFi/BLE 下载、串口、OTA/回滚和重连压力。

本次只增加核对记录，版本号与既有镜像保持 0.2.1 / 0.1.1，后续行为修复再发布新版本。
