> 本文包含开发阶段测量记录；其中工作区诊断脚本及测量产物仅在原本地工作区保留，不是本源码仓库的构建依赖。

# SPI2 SWD 实现与实测

日期：2026-11-15。ESP32-S3（240 MHz、ESP-IDF v6.1），USB CMSIS-DAP v1 HID，目标 STM32F401RCT6。原接线保持 GPIO4→PA14/SWCLK、GPIO5↔PA13/SWDIO、GPIO6→NRST、共地。示波器 SIGLENT SDS824X HD 的 CH1 接 SWCLK。

## 实现

新增 `main/probe_swd_spi.c` / `include/probe_swd_spi.h`，在 `probe_swd.c` 中增加可选后端，并注册控制台命令。`spi_bus_initialize(SPI2_HOST, ..., SPI_DMA_DISABLED)` 负责独占申请 SPI2；未注册普通 SPI device，短阶段直接使用 HAL/LL 和 CPU FIFO 轮询。SPI2 已被其他组件占用时启用失败，不接管其资源。SPI0/1 的 Flash/PSRAM 不参与。

GPIO matrix 将 GPIO5 同时接到 SPI MOSI 输出和接收输入，方向由 GPIO output-enable 显式切换，不增加方向控制线。SPI 使用 mode 0、LSB-first、半双工、无 CS、无自动 dummy；SWJ 线复位和模式切换序列保留 GPIO 实现。总线操作沿用 probe 锁串行执行。

每个事务先发 8 位请求，再接收 turnaround + 3 位 ACK。只有收到 OK 才进入 33 位数据/校验阶段；WAIT、FAULT 分别结束并交由原有上层处理。读事务在数据后补 turnaround，写事务在数据前补 turnaround。当前工程的数据阶段配置为 0，未新增 WAIT/FAULT 下强制数据阶段的支持。

ESP32-S3 的 FIFO 发送 33 位时不能直接按普通数据阶段处理；这里使用 **1 位 command + 32 位 data**，总共仍为连续 33 个时钟。1 位 idle 也使用 command。没有为了字节对齐插入额外数据时钟。各软件配置阶段间存在停顿；恢复默认高电平空闲时，以 SWDIO=0 输出一个合法 idle 上升沿，再恢复 SWDIO=1。因此段内 40 MHz 不代表整笔事务或 USB 吞吐为 40 Mbit/s。

接收路径有两种位对齐。自动模式在首个 DP IDCODE 请求上从 ACK 判断候选对齐，并以数据奇偶校验及 IDCODE 低 12 位 `0x477` 确认；频率改变后重新校准。低速路径会保留 ACK 阶段采到的数据首位，重建后续 32 位数据及校验，协议时钟数保持不变。提供手动覆盖用于诊断。

FIFO 轮询有超时预算。超时时记录错误、停用当前 SPI 状态、释放 SWDIO，以 GPIO 输入时钟结束可能仍由目标驱动的阶段，再恢复空闲。没有把超时或校验错误静默当作成功。

## 频率与控制台

```text
swdspi on          选择 SPI2；下一次连接仍按上位机请求的时钟设置
swd 40000          请求 40 MHz，不自行产生持续时钟
swdspi status      查看 actual_hz / calibrated / timeouts / parity_errors
swdspi align auto  自动接收位对齐（默认）
swdspi align 0     强制标准对齐，仅用于诊断
swdspi align 1     强制延迟对齐，仅用于诊断
swdspi off         恢复 GPIO / Dedicated GPIO
```

上电默认请求 1 MHz，SPI2 默认关闭；选择不写入 NVS。关闭 SPI 后保留总线申请供下次复用，重新初始化 SWD 或重启时释放。SPI2 资源不能同时交给其他外设。

使用 80 MHz 时钟源，预分频 1～16、计数分频 2～64，选择不超过请求值的最快组合，当前上限为 40 MHz。

| 请求 | 实际 SPI2 时钟 |
| --- | --- |
| 10 kHz | 普通 GPIO 回退，SPI 未启用 |
| 78.125 kHz | 78.125 kHz |
| 1 / 5 / 10 MHz | 1 / 5 / 10 MHz |
| 12 MHz | 11.428571 MHz |
| 15 MHz | 13.333333 MHz |
| 16 / 20 MHz | 16 / 20 MHz |
| 26.667 MHz | 26.666666 MHz |
| 40 / 50 MHz | 40 MHz |

`swdtiming` 中 `spi_selected=1` 表示用户选择了 SPI；`spi_enabled=1` 表示当前实际使用 SPI；`spi_hz` 是分频结果。低于 78.125 kHz 时明确回退普通 GPIO，并保留选择，之后可切回 SPI。GPIO 的 CCOUNT 数据段频率估计不用于报告 SPI 的物理频率。

## 验证记录

本轮 SPI2 测试使用的应用二进制：`firmware/build/esp_swdlink.bin`，1,132,464 字节，SHA256 如下。后续 H743 排障修复了 nRESET 位映射并重新构建，同名产物已更新；本节原始测试结果对应下列旧哈希，不能当作后来固件的重新验证。

```text
d3053a77a37e01e971268081293eeb88f72312a4510e7dfae6b0efc0f5a72407
```

已编译并烧录到 ESP32-S3。边界测试依次执行 10 kHz → 78.125 kHz → 15 MHz，确认 GPIO 回退、SPI 重新启用和向下分频均可读写。1、5、10、12、16、20、26.667、40、50 MHz 请求各 30 轮全部通过，FAULT、无效 ACK、奇偶校验和数据比较错误均为 0。合法 WAIT 单独统计，由上层重试。

最终固件 40 MHz 长测完成 **2,000 轮 / 128.099 秒**，比较 Flash **2,048,000 字节**，TAR 写入回读 **10,000 次**。ACK OK 602,009，WAIT 14,001（重试成功），FAULT、无效 ACK、奇偶校验错误均为 0。随后关闭 SPI，Dedicated GPIO 和普通 GPIO 各完成 30 轮读写回归，均通过，再恢复 SPI2。

pyOCD 以 `cortex_m`、40 MHz、attach 模式发现 1 个内核，读取 CPUID `0x410fc241`、Flash 容量 256 KB，并完成 100 次 4 KB Flash 读取比较（409,600 字节）。结束时目标为 RUNNING。最后控制台确认 SPI2 仍启用、实际 40 MHz、自动对齐已校准，超时和校验错误为 0，计时 profiling 已关闭。

原始结果：

- 低速边界与恢复（本地历史测量产物，不随源码分发）
- 最终固件扫频（本地历史测量产物，不随源码分发）
- 40 MHz 长测（本地历史测量产物，不随源码分发）
- Dedicated GPIO 回退（本地历史测量产物，不随源码分发）、普通 GPIO 回退（本地历史测量产物，不随源码分发）
- pyOCD 连接和读取回归及最终状态（本地历史测量产物，不随源码分发）

`tools/dev/swd_speed_test.py` 在 1 MHz 读取 4 KB Flash 基准，再于测试频率反复读取并比较，另外执行 DP IDCODE、CPUID 和 MEM-AP TAR 多种位模式写入回读。TAR 是调试地址寄存器；这些检查不擦写目标 Flash/SRAM，不暂停程序。因此验证覆盖 SPI 数据写路径，但不等同于目标 Flash 编程验证。STM32H743、Keil 和 OpenOCD 本次未复验。

示波器通过 `tools/dev/siglent_scope.py` 使用本机 VISA 获取 CH1 原始 WORD 波形，`tools/dev/swd_scope_capture.py` 生成读请求或带回读校验的 TAR 写请求并保存原始数据及图。抓取后自动停止流量，示波器保留 STOP 状态供查看。分析仅统计段内相邻上升沿，协议阶段间停顿不参与时钟频率估计。

最终固件的两次单次抓取使用约 0.5 ns 采样间隔（2 GSa/s）、100,000 点，结果如下。表内小数是阈值插值统计结果，不代表示波器绝对精度。

| 测量 | DP IDCODE 读流量 | TAR 写入并回读流量 |
| --- | --- | --- |
| 段内周期中位数 | 25.009 ns | 24.968 ns |
| 对应频率 | 39.986 MHz | 40.051 MHz |
| 高电平占比估计 | 49.09% | 49.52% |
| 周期的 5%～95% 分位 | 24.811～25.214 ns | 24.791～25.173 ns |
| 识别的正常段内周期 | 84 | 84 |
| 小于预期周期 60% 的间隔 | 0 | 0 |
| 抓取期间校验完成的操作 | 2,760 次 IDCODE 读取 | 1,820 次 TAR 写入，每批回读确认 |

读波形可见 8 / 4 / 33 个连续上升沿的阶段；写波形可见 8 / 4 / 1 / 33 个上升沿，最后的 33 位发送没有在 command/data 交界处增加间隙。单次波形不能证明所有未来事务都稳定，长测提供了独立的数据证据。

- 读波形图（本地历史测量产物，不随源码分发）、测量 JSON（本地历史测量产物，不随源码分发）
- 写波形图（本地历史测量产物，不随源码分发）、测量 JSON（本地历史测量产物，不随源码分发）
- 对应同名 `.preamble.bin` / `.waveform.bin` 为原始 SCPI 数据，`.npz` 为解析后的时间与电压。

波形中的振铃和过冲可能受用户指出的探头夹子耦合影响。CH1 只观测 SWCLK，单凭这些幅度不能归因到固件；频率测量与目标 ACK、奇偶校验、读写数据比较共同构成这里的验证证据。

## 参考与维护

- [Espressif SPI Master 官方文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html)：总线申请、GPIO 路由和 SPI 能力。本文实现直接依赖本机 IDF v6.1 的私有 HAL/LL 与 S3 寄存器布局，升级 IDF 须重新编译和进行硬件验证。
- [huming2207/swd-esp](https://github.com/huming2207/swd-esp)：SPI2 SWD 路线及 FIFO/接收对齐问题的参考。当前实现按本工程的单根双向 SWDIO 接线编写，并以本板实测为准。
- [SIGLENT SDS800X HD 编程手册](https://www.siglent.com/wp-content/uploads/2024/09/SDS800XHD_Series_ProgrammingGuide_EN11G.pdf) 与 [PyMeasure 的 SDS1000X HD 驱动源码](https://raw.githubusercontent.com/pymeasure/pymeasure/master/pymeasure/instruments/siglenttechnologies/siglent_sds1000xhd.py)：SCPI 波形获取及描述符字段参考。
