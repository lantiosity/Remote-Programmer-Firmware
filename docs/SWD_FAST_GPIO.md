> 本文包含开发阶段测量记录；其中工作区诊断脚本及测量产物仅在原本地工作区保留，不是本源码仓库的构建依赖。

# SWD 提速资料与专用 GPIO 实现

日期：2026-11-15。ESP32-S3 240 MHz，STM32F401RCT6，接线仍为 GPIO4/SWCLK、GPIO5/SWDIO、GPIO6/NRST。

后续进展：已另外实现 SPI2 后端，CH1 实测段内时钟约 40 MHz，见 [SPI2 实现与验证](SWD_SPI2.md)。本文保留 Dedicated GPIO 阶段的实现和历史测试；下文“本次未改为 SPI”仅指该阶段。

## 参考资料与路线选择

| 路线 | 一手资料 | 对当前工程的意义 |
| --- | --- | --- |
| CPU Dedicated GPIO | [Espressif 官方文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/dedic_gpio.html) | CPU 指令直接访问 GPIO，减少普通 GPIO 寄存器总线访问开销；初始化和操作必须在同一 CPU 核。本次优先采用。 |
| 专用 GPIO / SPI2 SWD | [huming2207/swd-esp](https://github.com/huming2207/swd-esp) | 已有专用 GPIO 与实验性 SPI2 HAL/LL 后端。其高速参考使用外部方向控制/电平转换电路，不能直接照搬接线或把作者的速度当作本板保证值。 |
| SPI 半双工 | [Espressif SPI Master 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html) | 支持 LSB-first、三线和半双工。短事务的配置/完成开销很重要；SWD 还必须先读 ACK 再决定后续时钟，不能按固定长度盲发。本次未改为 SPI。 |
| USB 吞吐优化 | [Arm CMSIS-DAP 固件说明](https://arm-software.github.io/CMSIS-DAP/latest/dap_firmware.html)、[包大小与缓冲数量配置](https://arm-software.github.io/CMSIS-DAP/latest/group__DAP__Config__Debug__gr.html) | CMSIS-DAP v2 使用 USB Bulk。若目标是减少烧录总耗时，后续应评估 Bulk、缓冲和流水线；SWCLK 提高不等于整体吞吐同比提高。 |

## 最终实现

- 通过 ESP-IDF 的 bundle 分配器申请 CPU1 的专用 GPIO 通道，查询实际通道偏移和输入掩码；未硬占其他组件的通道。
- 热路径使用 `ee.wr_mask_gpio_out`、`ee.get_gpio_in`，放在禁止内联的 IRAM 函数中。反汇编确认函数位于 `0x4037....` 区域，且含上述指令。
- DAP worker 原本已固定在 CPU1；控制台也固定到 CPU1。运行时的总线操作统一在 CPU1；CPU0 的启动流程仍初始化普通 GPIO，已激活的专用路径禁止从另一 CPU 退出。
- 在连接期间保持专用 GPIO 路由，避免逐 SWD 事务来回切换 matrix。更改频率、断开、电气诊断或交换引脚时，同步电平后恢复普通 GPIO。
- SWDIO 仍为同一根双向线，方向通过 GPIO output-enable 控制，增加完成屏障后才开始专用 GPIO 时钟。
- 保留固定的高电平 NOP 保护，防止连续、内联的 ACK 读之间出现过窄高脉冲。`swdfast 0` 仅表示零个可调的低电平额外 NOP，不是取消全部脉宽保护。
- 读写数据循环改为固定一位移位，避免每位动态移位索引。普通与专用后端共用 `probe_swd_transfer.inc` 协议体，保持 ACK、WAIT、FAULT、校验、turnaround 和 idle 的处理一致。

上电默认请求频率仍为 1 MHz；请求值超过 30 MHz 时，原有半周期延时为零，启用已准备好的专用 GPIO 后端。请求上限仍为 50 MHz，不能把该值当作实际波形频率。

## 验证方法与限制

`tools/dev/swd_speed_test.py` 在 1 MHz 捕获 Flash 基准，再比较高频读取结果，并对 MEM-AP TAR 做多个位模式的写入回读。该测试不擦写 STM32 Flash 或 SRAM，也不暂停目标程序。统计包含连接阶段的错误，WAIT 单独计数，FAULT、无效 ACK、校验错误和数据不一致均视为问题。

早期原型在约 8–12 MHz 的短测中曾通过，也在更长测试中出现过无效 ACK。因此不能用最初的短测结果认定稳定。当前结果仅适用于后续完成同步、脉宽保护和持续路由修改后的固件；这些修改联合验证，未对每一项做独立因果归因。

原普通 GPIO 固件由用户报告示波器周期 225 ns，约 4.44 MHz。新后端的频率来自 CPU 对 32 位数据段的计时，属于段平均估计；包括循环与中断开销，不是示波器读数，且各协议阶段的周期可能不同。

## 使用与回退

控制台设置：

```text
swdfast 0       启用专用 GPIO，低电平额外 NOP=0，仍保留固定高脉冲保护
swd 50000       最高请求档；不会自行产生持续时钟
swdtiming reset 开始采样统计
swdtiming       查看配置、读写数据段计时和 ACK 计数
swdtiming off   关闭并清空统计
swdfast off     恢复普通 GPIO 后端
```

`scan` 会自行改为扫描频率，不用于保持最高档。需要示波器观察时，应在设置最高档后另外产生读取流量。本次用户要求停止的持续输出保持关闭。

提速前固件备份：`firmware/build/esp_swdlink_before_dedicated.bin`。编译与烧录命令仍使用 `env.ps1`、`idf.py -C firmware -p COM20 flash`。

## 最终固件验证结果

最终固件大小 1,117,904 字节，SHA-256：`7d769bd93d605dd78664616a572da3a35206742d959b6a8d6ad914e9a9b01af9`。编译、烧录和 Flash 哈希校验通过。

最高 50 MHz 请求档、`swdfast 0` 的 2,000 轮压力测试通过：

- 耗时 128.111 秒，Flash 数据比较 2,048,000 字节，TAR 写入回读 10,000 次，数据一致。
- ACK OK 602,009 次；WAIT 14,001 次，重试成功；FAULT、无效 ACK、校验错误均为 0。
- 读数据段平均估计 **12.228 MHz**，最快完整段 12.308 MHz。
- 写数据段平均估计 **12.167 MHz**，最快完整段 12.347 MHz。
- 对应数据段平均周期约读 81.8 ns、写 82.2 ns。新波形尚未由用户用示波器复测。

额外验证：

- 普通 GPIO 回退模式在最高请求档通过 30 轮；重新开启专用 GPIO 后，1/10/30/50 MHz 请求档各通过 30 轮。
- 串口 `scan` 正确读到 DP IDCODE 0x2BA01477，验证断开与频率切换后普通路径仍可用。
- 关闭计时统计后，pyOCD USB attach 发现 1 个内核，CPUID 0x410fc241，4 KB Flash 块重复比较 100 次（409,600 字节）通过，目标最后为 RUNNING。
- 当前保留最高 50 MHz 请求设置、`swdfast 0`；统计已关闭，持续测量输出保持关闭。此为有限测试通过，不能保证所有接线、目标或环境条件。

同样的 2,000 轮测试，原普通后端耗时约 128.15 秒，新后端约 128.11 秒；总耗时几乎不变。因此能确认线上的数据段提速，不能宣称烧录速度已提升 2.7 倍。推测此负载中 USB HID/主机往返等固定开销更显著，进一步缩短烧录时间应单独评估 CMSIS-DAP v2 Bulk、包缓冲和流水线。

原始记录：

- `firmware/build/swd-dedicated-final-stress.json`
- `firmware/build/swd-dedicated-regression.json`
- `firmware/build/swd-dedicated-fallback.json`
- `firmware/build/swd-dedicated-final-pyocd.json`

复现最高档测试：

```powershell
python -u tools/dev/swd_speed_test.py --frequencies-mhz 50 --iterations 2000 --output firmware/build/swd-dedicated-final-stress.json
```
