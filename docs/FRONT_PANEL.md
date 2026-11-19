# ST7789、microSD 与六键面板

固件 0.1.4 在 ESP32-S3 N16R8 上启用 240×320 ST7789、原生 1-bit SDMMC 和六个独立按键。LCD 使用 SPI3，像素数据通过 DMA 从内部 RAM 发送。microSD 使用 SDMMC 控制器的 DMA，SPI2 可继续用于高速 SWD。

| 外设 | 模块端信号 | ESP32-S3 |
| --- | --- | --- |
| ST7789 | SCK / SCL | GPIO12 |
| ST7789 | MOSI / SDA / DIN | GPIO11 |
| ST7789 | CS | GPIO15 |
| ST7789 | DC | GPIO16 |
| ST7789 | RST / RES | GPIO21 |
| ST7789 | BL / BLK | GPIO47 |
| microSD | CLK | GPIO14 |
| microSD | CMD | GPIO13 |
| microSD | DAT0 | GPIO10 |
| 按键 | 上 / 下 | GPIO1 / GPIO2 |
| 按键 | 左 / 右 | GPIO39 / GPIO40 |
| 按键 | 确认 / 返回 | GPIO41 / GPIO42 |

- 所有外设与板子共地，信号使用 3.3 V。LCD 的 SDA 是 SPI 数据输入，MISO 无需接线。GPIO47 默认高电平点亮，连接模块的背光控制端。裸屏背光 LED 需按规格使用限流和驱动电路。
- microSD 使用能直接接触原生 SD 信号的 3.3 V 卡座或模块。CMD 和 DAT0～DAT3 各用 10 kΩ 上拉到 3.3 V。DAT1～DAT3 只在卡座侧上拉，无需连接 ESP32 GPIO。CLK 直连，卡检测脚留空。
- 每个按钮一端接指定 GPIO，另一端接 GND。固件启用内部上拉与按下消抖。GPIO39～42 用于按键时，板上这组外接 JTAG 引脚不能同时使用。
- LCD 按 240×320 竖屏、SPI mode 0、20 MHz 和默认反色驱动。若背光极性或色彩与模块不符，可在 `idf.py menuconfig` 的本地面板菜单调整背光极性、反色及屏幕时钟。该菜单也可关闭整套面板。

## 使用

开机尝试挂载 `/sd`。使用 FAT32 卡。若开机未插卡，插入后按确认键挂载。屏幕显示卡状态、容量以及根目录的前 16 项，长文件名在屏幕上截短。上、下键移动选择，确认键刷新目录，返回键卸载卡。屏幕显示 `SD: EJECTED` 后可拔出。左右键切换卡页面和按键计数页面。

本版只读取目录，可用于验证接线和存储读取。挂载失败时保留原卡内容，不自动格式化。STM32 离线烧录未接入。LCD 或卡初始化报错可在 CH343 控制台查看 `front_panel` 日志。面板初始化和读卡在独立任务执行，卡缺失不会阻止原有调试与 OTA 启动。
