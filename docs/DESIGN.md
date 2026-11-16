# 架构与协议说明

## 1. 总体结构

```
                ┌──────────────────────── ESP32-S3 ────────────────────────┐
  PC  ──USB──►  │ TinyUSB 复合设备                                        │
  HID  CMSIS-DAP│   ITF0: HID 64B 收发   ITF1/2: CDC ACM (虚拟串口)        │
  CDC  虚拟串口 │        │                       │                         │
                │        ▼                       ▼                         │
                │   link_submit()           serial_bridge ◄── UART1 ──► 目标串口
                │        │                       ▲                         │
  PC ──TCP5555► │   TCP 服务(JSON 行) ───────────┘                         │
  PC ──TCP5556► │   TCP 串口桥                                             │
  PC ──BLE───►  │   NimBLE: DAP 服务 / NUS 服务                            │
                │        │                                                 │
                │        ▼                                                 │
                │   DAP 工作线程（串行执行，队列深度 8）                    │
                │     ├── USB 来源 → dap_cmsis.c（CMSIS-DAP 命令）         │
                │     └── TCP/BLE 来源 → dap_server.c（pyOCD JSON 协议）   │
                │                 │                                        │
                │                 ▼                                        │
                │            probe_core.c（DP/AP/MEM-AP/复位）             │
                │                 │                                        │
                │                 ▼                                        │
                │            probe_swd.c（GPIO / Dedicated GPIO / SPI2）    │
                └──────────────────────────────────────────────────────────┘
                                    │ SWCLK / SWDIO / NRST
                                    ▼
                                  目标 MCU
```

要点：

- **SWD 只有一套硬件**，所以所有链路的请求都进同一个队列，由 `dap_worker` 串行执行；
  一个请求执行完按"来源"回送响应。
- USB 走 CMSIS-DAP 二进制命令（Keil/OpenOCD 只认这个）；WiFi/BLE 走 pyOCD 的远程探针
  JSON 协议（pyOCD 自带 `remote` 插件，不需要写 PC 端插件）。

## 2. 源码模块

| 文件 | 作用 |
| --- | --- |
| `main.c` | 初始化顺序：NVS → 配置 → SWD → link → USB → 串口桥 → WiFi → BLE → 控制台 |
| `app_cfg.c` | Kconfig 默认值 + NVS 运行时覆盖（WiFi 凭据、波特率）、序列号 |
| `probe_swd.c` | 寄存器级 GPIO 位操作：请求/ACK/数据/校验/turnaround、线复位、SWJ_Pins |
| `probe_swd_spi.c` | 可选 SPI2 FIFO/LL 后端，按 ACK 分支执行读写阶段，保留 GPIO4/5 接线 |
| `probe_core.c` | DP/AP 传输、AP posted-read 处理、MEM-AP 内存/块访问、复位 |
| `dap_cmsis.c` | CMSIS-DAP v1 命令层（64 字节报文），给 Keil/pyOCD/OpenOCD 用 |
| `dap_server.c` | pyOCD remote 探针 JSON 协议服务端 |
| `link.c` | 请求队列 + DAP 工作线程 + 各链路发送函数注册 |
| `usb_dev.c` | TinyUSB 描述符、HID 收发回调、CDC 虚拟串口 |
| `net_dev.c` | WiFi STA+AP、TCP 5555（DAP）/5556（串口） |
| `ble_dev.c` | NimBLE GATT：DAP 服务 + NUS 服务 |
| `ble_ota.c` | 共享 ESP32 OTA worker：BLE GATT、SHA-256、会话互斥、非活动槽写入、启动确认 |
| `wired_ota.c` | 原生 USB HID 厂商命令及 WiFi TCP 5557 OTA 适配；沿用同一校验与回退流程 |
| `serial_bridge.c` | 目标 UART ↔ CDC / BLE NUS / TCP 的双向桥 |
| `console_cmds.c` | esp_console 命令（status/wifi/wifihex/swd/scan/rst/baud/reboot） |

## 3. USB 设备描述符

| 项 | 值 |
| --- | --- |
| VID:PID | `0x0D28:0x0204`（ARM DAPLink 常用 ID，可用 menuconfig 改） |
| 设备类 | `0xEF/0x02/0x01`（Misc / IAD） |
| 接口 0 | HID，Usage Page `0xFF00`、Usage `0x01`，64 字节 IN/OUT，**无 Report ID** |
| 接口 1/2 | CDC ACM（含 IAD），用于虚拟串口 |
| 端点 | `0x01/0x81` HID OUT/IN，`0x02/0x82` CDC 数据，`0x83` CDC 通知 |
| 产品字符串 | `ESP32-S3 CMSIS-DAP`（必须含 "CMSIS-DAP"，pyOCD/OpenOCD 靠这个识别） |
| 序列号 | `ESP-SWD-xxxxxx`（由 MAC 生成） |

CMSIS-DAP v1 用 HID 传输的好处：Windows 免驱，Keil/OpenOCD 开箱即用。

## 4. CMSIS-DAP 命令支持

| 命令 | 码 | 支持情况 |
| --- | --- | --- |
| DAP_Info | 0x00 | ✅ 厂商/产品/序列号/版本/能力/包大小；字符串响应带结尾 `\0`（长度含 `\0`） |
| DAP_HostStatus | 0x01 | ✅ 空实现（返回 OK） |
| DAP_Connect | 0x02 | ✅ 仅 SWD（port=0/1），JTAG 返回失败 |
| DAP_Disconnect | 0x03 | ✅ |
| DAP_TransferConfigure | 0x04 | ✅ idle / wait_retry / match_retry |
| DAP_Transfer | 0x05 | ✅ 多条 DP/AP 读写 |
| DAP_TransferBlock | 0x06 | ✅ 含 AP posted-read 的 RDBUFF 收尾 |
| DAP_TransferAbort | 0x07 | ✅（同步实现，置位即可） |
| DAP_WriteABORT | 0x08 | ✅ 写 DP ABORT（清 sticky error） |
| DAP_Delay | 0x09 | ✅ |
| DAP_ResetTarget | 0x0A | ✅ 拉低 NRST 20ms 后重新线复位；无 NRST 时用 SYSRESETREQ |
| DAP_SWJ_Pins | 0x10 | ✅ SWCLK/SWDIO/NRESET |
| DAP_SWJ_Clock | 0x11 | ✅ 请求设置 10 kHz–50 MHz，实际频率受 GPIO 执行开销限制 |
| DAP_SWJ_Sequence | 0x12 | ✅ 最多 256 位（0 表示 256） |
| DAP_SWD_Configure | 0x13 | ✅ 返回 OK（turnaround 固定 1 周期） |
| DAP_JTAG_* | 0x14~0x16 | ⛔ 返回 OK 但不工作（只支持 SWD） |
| DAP_SWO_* | 0x17~0x1D | ⛔ 返回错误（未实现 SWO） |
| DAP_ExecuteCommands | 0x7F | ⛔ 返回错误（v2 批量命令） |

`DAP_Info` 能力位只返回 `0x01`（仅 SWD）。注意 bit6 在规范里是 *Atomic Commands*，不是 SWD_Sequence，不能乱置位。

> 踩过的坑（已修）：命令码不能想当然（`DAP_Transfer` 是 0x05 不是 0x14），
> `DAP_Info` 的 `0xF0=能力`、`0xFE=包个数`、`0xFF=包大小`，字符串要带 `\0`。

## 5. pyOCD remote 探针协议（WiFi / BLE）

一行一条 JSON：

```json
请求: {"id":3,"request":"read_dp","arguments":[0]}
响应: {"id":3,"status":0,"result":721346687}
错误: {"id":3,"status":12,"error":"DP read failed"}
```

状态码沿用 pyOCD：`0` 成功、`1` 通用错误、`3` 探针错误、`10` 传输错误、`11` 超时、`12` FAULT。

已实现的请求：

```
hello, readprop, open, close, lock, unlock, connect, disconnect,
swj_sequence, set_clock, reset, assert_reset, is_reset_asserted, flush,
read_dp, write_dp, read_ap, write_ap, read_ap_multiple, write_ap_multiple,
get_memory_interface_for_ap, read_mem, write_mem,
read_block32, write_block32, read_block8, write_block8,
has_swo（返回 false）, swo_*（返回错误）, jtag_sequence（返回错误）
```

`readprop` 支持：`unique_id`、`vendor_name`、`product_name`、`description`、
`supported_wire_protocols`、`wire_protocol`、`capabilities`。

参数大小：TCP 一次最多收 20KB 的 JSON 行（够 `write_block32` 写 1KB 页），
`read_block32` 单次最多 2048 字（8KB）。

## 6. 蓝牙 GATT

| 用途 | UUID | 属性 |
| --- | --- | --- |
| DAP 服务 | `6e400010-b5a3-f393-e0a9-e50e24dcca9e` | — |
| DAP RX | `6e400011-...` | Write / WriteNoRsp |
| DAP TX | `6e400012-...` | Notify |
| NUS 服务 | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | — |
| NUS RX | `6e400002-...` | Write / WriteNoRsp |
| NUS TX | `6e400003-...` | Notify |

- 广播包只放名字 `ESP-SWD`（两个 128 位 UUID 放不进 31 字节广播包），
  客户端连上后按 UUID 发现服务。
- 设备申请 MTU 512；响应按 `MTU-3`（上限 244）分片，PC 侧按 `\n` 重组。
- 请求也是按 `\n` 重组后整行送给 `dap_server.c`。

## 7. 串口桥

```
UART1 (GPIO17/18)
   │ uart_read_bytes
   ├─► USB CDC（tud_cdc_write）
   ├─► BLE NUS 通知
   └─► TCP 5556 客户端
   ▲
   ├─ USB CDC 读
   ├─ BLE NUS 写
   └─ TCP 5556 收
```

订阅者用 `serial_bridge_subscribe(fn, opaque)` 注册，最多 4 个。

## 8. SWD 时序实现要点

- **可选 SPI2**：`swdspi on` 申请 SPI2 独占总线并启用 FIFO 轮询，SWJ 序列仍由 GPIO 执行。SPI mode 0、LSB-first、手动控制 SWDIO 输出使能；分阶段发送请求、读取 ACK，再决定后续方向和时钟。使用 80 MHz 源向下分频，当前实际最高 40 MHz，低于 78.125 kHz 时使用普通 GPIO。读路径通过首个有效 DP IDCODE 自动校准接收位对齐；33 位写路径使用 1 位 command + 32 位 FIFO data。详见 [SPI2 SWD](SWD_SPI2.md)。
- **时钟**：`GPIO_OUT_W1TS/W1TC` 直接置位/清零，半周期用 NOP 循环；
  `1000 kHz` 设置对应每半周期 30 次 NOP 循环（CPU 240 MHz）；超过 30 MHz 的请求可启用 CPU1 Dedicated GPIO 后端，热路径置于 IRAM，仍保留高脉冲宽度保护。
  `swdtiming` 使用 CPU CCOUNT 统计实际读写事务中 32 位数据段的执行时间；专用 GPIO 最高档估计读约 12.23 MHz、写约 12.17 MHz。这是段平均计时估计，不能替代示波器测量或当作连续、固定占空比的时钟。详见 `SWD_FAST_GPIO.md`；此前普通 GPIO 版本的测试保留在 `SWD_CLOCK_LIMIT_TEST.md`。
- **请求字**：上位机给的 8 位请求原样按 LSB 先出（起始位、APnDP、RnW、A[3:2]、
  校验、停止位、空闲位）。
- **方向切换**：发完请求立即释放 SWDIO；ACK 的 3 个位用"拉高→等待→采样→拉低"读取；
  读数据 32 位 + 校验位后主机收回总线补 1 个空闲时钟；写则先补 turnaround 再输出。
- **AP posted read**：AP 读到的总是上一次的数据，所以
  `probe_ap_read()` 会再读一次 DP `RDBUFF`；`probe_transfer_block()` 对 AP 块读
  做了"读 count 次 + 末尾 RDBUFF"的移位修正。
- **MEM-AP**：`CSW = size | 0x10(SingleAddrInc) | 0x40(DeviceEn)`，
  8/16 位访问按地址对齐移位（与 pyOCD 的 `ap.py` 行为一致）；块访问靠 TAR 自增。
- **WAIT 重试**：`DAP_TransferConfigure` 设置的 `wait_retry` 次。

## 9. 任务与内存

| 任务 | 栈 | 优先级 | 说明 |
| --- | --- | --- | --- |
| `dap_worker` | 8KB | 12 | 串行执行 DAP 请求 |
| `tusb` (esp_tinyusb) | 4KB | 10 | USB 协议栈 |
| `tcp_dap` | 6KB | 9 | DAP TCP 服务（单客户端） |
| `tcp_serial` | 4KB | 7 | 串口 TCP 服务 |
| `serial_bridge` | 4KB | 8 | UART ↔ CDC/各路订阅者 |
| NimBLE host | 默认 | — | GATT 服务 |
| esp_console REPL | 5KB | — | 控制台 |

静态缓冲：`dap_server` 的 8KB 块缓冲 + 8KB 字节缓冲、JSON 响应 20KB（工作线程静态）、
TCP/BLE 接收行缓冲各 20KB（BLE 是静态，TCP 是 malloc）。

## 10. 配置项（menuconfig）

```
ESP-SWD DAPLink 配置
├── 1. SWD 引脚与调试口     PROBE_SWCLK_GPIO / PROBE_SWDIO_GPIO / PROBE_NRESET_GPIO / PROBE_DEFAULT_CLOCK_KHZ
├── 2. 目标串口             TARGET_UART_PORT / TX / RX / BAUD
├── 3. USB 设备             USB_DEVICE_VID / PID / MANUFACTURER / PRODUCT
├── 4. WiFi 链路            LINK_WIFI_SSID / PASSWORD / AP_SSID / AP_PASSWORD / TCP 端口
└── 5. 蓝牙链路             LINK_BLE_ENABLE / LINK_BLE_NAME
```

运行时还有 NVS 覆盖：WiFi 凭据（`wifi` / `wifihex` 命令）和目标串口波特率（`baud`）。

## 11. 已知问题 / 待办（来自代码审阅）

已修：

- `read_ap`/`write_ap` 之前用 `addr & 0x0F`，丢掉 APBANKSEL → pyOCD 读 AP IDR(0xFC) 会读到 DRW，WiFi/BLE 烧写必然失败。现在保留 `A[3:2]` 并把 `addr & 0xF0` 写进 `DP_SELECT` 的 APBANKSEL。
- `probe_mem_read_block8` 非对齐起始地址错位。
- `DAP_Info` 能力位从错误的 `0x41` 改为 `0x01`。
- `DAP_Transfer` 在 ACK 失败时仍追加 4 字节数据 → 上位机解析错位。
- 响应缓冲溢出时返回半截 JSON → 现在返回 `status=1` 且数据量上限由缓冲反推。
- SWD 总线加递归互斥锁（`probe_lock/unlock`），控制台诊断命令与 `dap_worker` 不再打架。
- GPIO 编号做 `SOC_GPIO_PIN_COUNT` 范围检查。
- `probe_connect` 仅在 ACK 成功时标记 connected。

- **BLE 通知跨任务**：改为投递 npl 事件到 NimBLE host 默认事件队列，真正发送在 host 任务里做；分片发送在 `BLE_HS_ENOMEM` 时按 1ms 退避重试（背压），仍失败则丢弃剩余并打日志。
- **CDC 并发**：`usb_dev_cdc_read/write` 改用 esp_tinyusb 的 `tinyusb_cdcacm_read/write_queue/write_flush`（内部有临界区）。
- **TCP 会话**：`link_submit` 的 opaque 改成 `fd | (epoch<<16)`，`tcp_dap_send` 同时校验 fd 和会话代号，fd 复用不会写错连接。
- **串口订阅表**：`s_sinks` 加互斥锁，取快照后在锁外调用回调；客户端 fd 设 `SO_SNDTIMEO=1s`，对端不读不会卡死 UART 转发。
- **丢包可观测**：HID IN 忙导致的丢弃计入 `link_stats.dropped`。

待办：

- `wirelink` 增加 `selftest`（`read_ap((1<<24)|0xFC)`、内存回环），把 AP 银行类 bug 钉在回归测试里。
- `probe_swd_output_sequence` 的无效参数清理、`DAP_SWD_Sequence(0x1D)` 支持。
- 目标板 SWD 应答问题（物理层）尚未闭环。
