# ESP32 自身升级（0.1.2）

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

1. 选择发布包 `release/0.1.2/images/esp_swdlink.bin`，或自行构建的同名应用。
2. 停止其他烧录/调试任务，连接对应 USB、WiFi 或 BLE 链路。
3. 运行状态下长按 BOOT 约 2 秒开放升级窗口，或在设备控制台执行 `ota arm`（默认 300 秒）。
4. 在上位机的烧录器升级功能读取信息，再执行升级并等待设备重启。

固件写入非运行 OTA 槽，校验文件长度、SHA256、ESP 镜像及项目名后才允许激活。中断或校验失败不会选中新槽；重新连接后从头传输。初始化完成约 5 秒后确认新镜像，未确认即复位时由 bootloader 回退。

`idf.py flash` 是完整安装流程，会重新写入初始 otadata；需要保留现有运行槽时使用 OTA。NVS、PHY 和 offline 不应作为 OTA 目标。

SHA256 是完整性检查，当前协议不提供签名认证。授权窗口控制入口。详细实现见 `main/ble_ota.c`、`main/wired_ota.c` 及对应头文件。

## 0.1.1 授权与兼容性

- 原生 USB 的 OTA 授权指令只开放 USB，不能顺带开放无线更新。保持 USB HID / WLO1 协议 v1；应用镜像和分区布局不变（layout 2）。
- WiFi：长按运行中的 BOOT 约 2 秒，或 UART 控制台 `ota arm 300 wifi`。BLE：物理按钮或 `ota arm 300 ble`；`ota arm 300 usb` 只开放 USB，`ota arm 300 all` / 原有 `ota arm` 开放全部链路。无线配网保存仅认可包含 BLE 的窗口，USB 配网仍可直接保存。
- BLE OTA 写特征默认要求加密（`BLE_OTA_REQUIRE_ENCRYPTION=y`），首次需通过操作系统配对/建立加密连接。Just Works 不提供 MITM 身份保证，窗口期间应只允许可信客户端接近设备；当前镜像仍没有签名认证。可在 menuconfig 显式关闭该要求用于受控旧客户端兼容，但默认发布包保持启用。
- 接收空闲超时默认 120 秒，FINISH 校验后等待激活默认 600 秒，分别由 `OTA_IDLE_TIMEOUT_SECONDS` / `OTA_READY_TIMEOUT_SECONDS` 配置。TCP 端口由 `LINK_OTA_TCP_PORT` 配置，默认仍为 5557，设备信息同步报告实际端口。
- USB OTA 请求仍在 DAP worker 中等待共享 OTA worker 提交，最长 15 秒，但这段等待不持有 SWD 总线锁；升级期间请停止其他烧录/调试任务。

0.1.1 已完成构建和本机协议/内存/串口回归，本次未实测物理设备 OTA、配对和回滚。进入 ROM 下载模式进行完整安装的说明见 [README](../README.md#直接烧录发布固件无需编译)。

0.1.2 中 OTA status 的 Flags bit0 及 ARMED 状态只反映当前查询链路；未授权链路返回未授权/IDLE，其他活动会话仍如实显示繁忙。控制台的状态保留全局视角。配网 status 的 armed 同样区分 USB/BLE，避免先前仅开放 USB 后无线预检查误通过。

配网保存或清除从写入 NVS 前一直保留重启权限，直到定时重启。OTA 已在进行时，配网和控制台重启返回繁忙。已保留重启权限时，固件拒绝 OTA 开始请求并在 OTA 状态中报告繁忙。

清除配网时，固件先提交空的 `wifi_v1` 记录，再按 `ssid`、`pass` 的顺序删除旧版键，并在持久化完成后安排重启。旧键已不存在时可继续清除，持久化出错时可重试。
