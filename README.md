# ESP32-S3 远程烧录器固件

独立固件源码仓库，配套 WireLink 上位机 **0.2.3**。固件支持 CMSIS-DAP、SWD、USB/WiFi/BLE 链路、目标 UART 桥、ESP32 自身 OTA 和 WiFi 配置。

当前源码为 **0.1.4**。下方可下载包仍为 **0.1.3**。开发版新增的本地面板和网页配置需要从源码构建。

## 直接烧录发布固件（无需编译）

可烧录文件位于 [`release/0.1.3/images/`](release/0.1.3/images/)，也可解压 [`firmware-0.1.3-flash.zip`](release/0.1.3/firmware-0.1.3-flash.zip) 单独使用。烧录对象是 **ESP32-S3 N16R8**（16 MiB Flash、8 MiB Octal PSRAM）。

1. 安装 Python 和 esptool：`python -m pip install esptool==5.6.0`。已激活 ESP-IDF 环境的用户可使用其自带 esptool。
2. 用数据线连接 ESP32-S3 的原生 USB/OTG 口，按住 BOOT，按一下 RESET，再松开 BOOT；在设备管理器查看新出现的 ROM 下载串口。原生 USB 可以完成整套烧录，无需另接 USB 转串口。
3. 在固件仓库目录打开 PowerShell，执行以下命令，把 `COM22` 换成实际下载端口：

```powershell
cd release/0.1.3/images
python -m esptool --chip esp32s3 --port COM22 --before no-reset --after hard-reset write-flash "@flash_args"
```

保持 `flash_args` 与各镜像的相对路径不变。esptool 会逐项写入并校验；完成后松开 BOOT，如仍停留在 ROM 下载口，只按一下 RESET。应用启动后应枚举 CMSIS-DAP HID 和 CDC 串口，端口号可能变化。USB-UART/CH343 口也可在手动进入下载模式后使用同一命令，改为对应 COM 口。

| 文件（相对 images 目录） | 写入地址 | 用途 |
| --- | --- | --- |
| `bootloader/bootloader.bin` | `0x0` | ESP32-S3 引导程序 |
| `partition_table/partition-table.bin` | `0x8000` | 16 MiB 分区表 |
| `esp_swdlink.bin` | `0x10000` | factory 应用；也用于日常 OTA |
| `ota_data_initial.bin` | `0x310000` | 初始化 OTA 选择，启动 factory |

完整安装会覆盖 factory 和 OTA 选择数据，重新从 factory 启动；命令按地址分别写入，不擦除 NVS、PHY 和 offline 预留区。已有设备应先备份重要数据，且不要启用全片擦除。factory 为 3 MiB，两个 OTA 槽各 4 MiB，offline 保留 4.875 MiB，后者尚未实现离线烧录功能。分区变化必须走完整安装，不能只传应用 OTA。

日常 USB/WiFi/BLE OTA 只选择 [`esp_swdlink.bin`](release/0.1.3/images/esp_swdlink.bin)。升级步骤、镜像清单及校验方式见 [OTA 操作说明](docs/OTA.md)和[烧录说明](release/0.1.3/images/README.md)。

## 构建

使用 ESP-IDF 6.1、ESP32-S3 N16R8（16 MiB Flash、8 MiB Octal PSRAM）。ESP-IDF 和工具链由使用者在仓库外安装，激活官方 ESP-IDF 环境后，在本目录执行：

```powershell
idf.py set-target esp32s3
idf.py build
```

组件由 `main/idf_component.yml` 声明、`dependencies.lock` 锁定；首次构建可能联网下载。`sdkconfig.defaults` 是可发布的默认配置；本机 `sdkconfig`、`build/`、`managed_components/` 不纳入版本或源码 release。其他板型需要通过 `idf.py menuconfig` 修改 PSRAM、Flash 模式及引脚。源码版本在 `CMakeLists.txt` 设置。

默认 SWCLK=GPIO4、SWDIO=GPIO5、nRESET=GPIO6；目标 UART TX=GPIO17、RX=GPIO18；BOOT=GPIO0。请共地，按目标电平接线。

首次安装使用 ROM 下载模式，连接 ESP32 自身 USB/UART，按住 BOOT 并复位，识别下载端口后使用 `idf.py -p COMx flash`。这会写 bootloader、分区表、factory 和初始 otadata；已有设备升级前应备份持久化数据。日常升级在配套上位机选择本次构建的 `build/esp_swdlink.bin`，参见 [升级说明](docs/OTA.md)。

## 配置与源码

WiFi 凭据通过运行时配网保存到设备 NVS，不提交到源码。默认热点名 `ESP-SWD`、密码 请在 menuconfig 设置独立口令，部署时按需要修改。长按 BOOT 约 2 秒开启本地配置/升级授权窗口；不要同时复位。

`main/` 为固件实现，`partitions.csv` 是 16 MiB 分区布局。USB、BLE 和 CH343 控制台的配网协议共用 `wifi_setup.c`，设备信息以 `wifi_setup: 1` 声明 USB 配网能力。

源码 ZIP、镜像与 SHA256 见 [0.1.3 发布说明](release/0.1.3/README.md)，版本变化见 [CHANGELOG](CHANGELOG.md)。

## 运行行为与验证

开发版支持写入型 SPI3 ST7789 240×320、独立 1-bit SDMMC 与六个按键。LCD 和 SDMMC 均使用 DMA。详见[面板接线与操作](docs/FRONT_PANEL.md)。

开发版可通过浏览器查看设备状态和配置路由器 Wi-Fi。连接设备热点后打开 `http://192.168.4.1/`，或在同一局域网打开设备当前 IPv4 地址。修改前长按 BOOT 约 2 秒读取 LCD 配置码，或在 UART 控制台运行 `webcfg arm`。详见[网页配置说明](docs/WEB_CONFIG.md)。

SWD 支持按连接独占，配网与 OTA 互斥，UART 使用独立发送队列。配置为无 nRESET 时，探针复位返回错误，软件复位由调试器选择目标核后发起。

USB 发起的 OTA 授权只开放 USB；WiFi/BLE 升级请长按 BOOT 约 2 秒，或在 UART 控制台使用 `ota arm 300 wifi` / `ota arm 300 ble`。BLE OTA 默认要求加密连接，首次请在操作系统完成配对；详见 [OTA](docs/OTA.md)。

原生回归（需要 gcc 与已解析的 cJSON 组件，无需目标板）：

```powershell
python tests/run_native.py
```

`selftest <APSEL>` 读取 AP IDR。`selftest <APSEL> <scratch_RAM_address>` 在用户指定、已暂停目标的 32 字节空闲 RAM 上执行备份、写读与恢复。不要向该命令提供外设或 Flash 地址。低波特率串口压力、BLE 配对、USB/WiFi 下载、OTA 回滚和 SPI2 波形仍待实板验收。
