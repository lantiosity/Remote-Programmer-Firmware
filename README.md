# ESP32-S3 远程烧录器固件 0.1.0

独立固件源码仓库，首版编号为 **0.1.0**。支持 CMSIS-DAP、SWD、USB/WiFi/BLE 链路、目标 UART 桥、ESP32 自身 OTA 和 USB/BLE WiFi 配置。配套 WebView2 上位机首版为 0.2.0，在另一个仓库维护。

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

`main/` 为固件实现，`partitions.csv` 是 16 MiB 分区布局。USB/WiFi/BLE 配网共用 `wifi_setup.c`；设备信息中的 `wifi_setup: 1` 用于能力识别，避免版本重新从 0.1.0 编号导致旧的大小比较误判。

`release/0.1.0/` 保存首版源码快照、逐文件清单及 SHA256。后续新增版本目录，不覆盖历史归档。该归档不含工具链、依赖缓存或预编译镜像。此次重新编号不代表已将物理设备重刷为 0.1.0。
