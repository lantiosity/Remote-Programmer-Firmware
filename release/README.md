# 固件发布包

[ESP-SWD 0.1.5](0.1.5/README.md) 为本次公开发布候选，配套 WireLink 0.3.1，适用 ESP32-S3 N16R8。

首次烧录使用完整烧录 ZIP。正常 OTA 选择对应版本的 `images/esp_swdlink.bin`。解压和烧录步骤见[固件 README](../README.md#直接烧录发布固件无需编译)。

更新版本及文档、通过原生回归和 ESP-IDF 构建后，暂存源码并执行 `python tools/archive-source.py 版本号`。提交源码后执行 `python tools/package-images.py 版本号`，校验来源提交、芯片、分区和校验和。
