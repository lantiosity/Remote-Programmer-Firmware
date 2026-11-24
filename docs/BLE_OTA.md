# BLE OTA 协议 v1

固件 0.1.5 使用 BLE OTA 协议 v1，布局与操作见 [升级说明](OTA.md)。控制和数据写入默认要求 BLE 加密。所有 UUID 以 `-b5a3-f393-e0a9-e50e24dcca9e` 结尾。

| UUID 前缀 | 用途 |
| --- | --- |
| 6e400020 | OTA Service |
| 6e400021 | Control，write with response |
| 6e400022 | Data，write with response |
| 6e400023 | Status，read/notify |
| 6e400024 | Info，JSON |

Control：BEGIN=`01 + size:u32 + sha256:32bytes`，FINISH=`03`，ACTIVATE=`04`，ABORT=`05`。Data=`offset:u32 + 数据`，整数小端，负载按协商 MTU 缩小。FINISH 校验成功后进入 READY，ACTIVATE 才切换启动槽并重启。Info 包含协议、项目、版本、槽及能力信息；`wifi_setup:1` 表示 USB 配网可用。

默认授权按钮 GPIO0，长按约 2 秒；控制台 `ota status` 查询状态，`ota arm [秒数]` 开放窗口（1～600 秒），`ota off` 关闭。空闲超时、断线或取消释放升级会话，不支持断点续传。升级时停止其他调试业务。

状态布局及错误码以 `main/include/ble_ota.h`、`main/ble_ota.c` 为准。BLE 配网使用另一个服务（前缀 6e400030），请求和应答特征均要求链路加密。保存和清除仍须开放包含 BLE 的物理授权窗口。默认在 NVS 保存配对密钥，以便重启后重新建立加密连接。
