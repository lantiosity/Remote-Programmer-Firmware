# ESP32 自身升级（0.1.0 首版）

USB（CMSIS-DAP HID 扩展）、WiFi 和 BLE 共用 OTA 状态机。升级对象为 `build/esp_swdlink.bin`，不是目标 MCU 固件。配套上位机在独立仓库维护。

## 分区

| 分区 | 起始地址 | 大小 |
| --- | --- | --- |
| nvs | 0x9000 | 0x6000 |
| phy_init | 0xf000 | 0x1000 |
| factory | 0x10000 | 0x300000 |
| otadata | 0x310000 | 0x2000 |
| ota_0 | 0x320000 | 0x400000 |
| ota_1 | 0x720000 | 0x400000 |
| offline | 0xb20000 | 0x4e0000 |

以 `partitions.csv` 为准。分区变更不能通过应用 OTA 完成，必须进入 ROM 下载模式迁移完整布局。不要对不同布局的设备直接套用应用镜像。

## 操作

1. 用 ESP-IDF 构建，选择 `build/esp_swdlink.bin`。
2. 停止其他烧录/调试任务，连接对应 USB、WiFi 或 BLE 链路。
3. 运行状态下长按 BOOT 约 2 秒开放升级窗口，或在设备控制台执行 `ota arm`（默认 300 秒）。
4. 在上位机的烧录器升级功能读取信息，再执行升级并等待设备重启。

固件写入非运行 OTA 槽，校验文件长度、SHA256、ESP 镜像及项目名后才允许激活。中断或校验失败不会选中新槽；重新连接后从头传输。初始化完成约 5 秒后确认新镜像，未确认即复位时由 bootloader 回退。

`idf.py flash` 是完整安装流程，会重新写入初始 otadata；需要保留现有运行槽时使用 OTA。NVS、PHY 和 offline 不应作为 OTA 目标。

SHA256 是完整性检查，当前协议不提供签名认证。授权窗口控制入口。详细实现见 `main/ble_ota.c`、`main/wired_ota.c` 及对应头文件。
