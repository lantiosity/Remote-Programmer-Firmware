# 固件本地打包

打包脚本会生成适用于 ESP32-S3 N16R8 的完整烧录 ZIP。OTA 使用 `images/esp_swdlink.bin`。从源码构建和烧录的步骤见[固件 README](../README.md#从源码烧录固件)。

更新版本及文档、通过原生回归和 ESP-IDF 构建后，暂存源码并执行 `python tools/archive-source.py 版本号`。提交源码后执行 `python tools/package-images.py 版本号`，校验来源提交、芯片、分区和校验和。
