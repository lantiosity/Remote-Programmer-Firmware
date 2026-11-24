# 固件版本归档

当前固件为 **[0.1.4](0.1.4/)**，配套上位机 0.3.0。历史归档从 [0.1.0](0.1.0/) 起保留。

0.1.4 于 2026-11-24 随复审修复重新发布。核对镜像时请使用当前 0.1.4 发布目录的来源提交和 SHA256。早期撤回包保留在工作区历史归档。

| 版本 | 独立 OTA 应用镜像 |
| --- | --- |
| 0.1.0 | [esp_swdlink.bin](0.1.0/images/esp_swdlink.bin) |
| 0.1.1 | [esp_swdlink.bin](0.1.1/images/esp_swdlink.bin) |
| 0.1.2 | [esp_swdlink.bin](0.1.2/images/esp_swdlink.bin) |
| 0.1.3 | [esp_swdlink.bin](0.1.3/images/esp_swdlink.bin) |
| 0.1.4 | [esp_swdlink.bin](0.1.4/images/esp_swdlink.bin) |

USB、WiFi 或 BLE OTA 请选择对应版本的应用镜像。今后的每个发布目录也保留 `images/esp_swdlink.bin` 和其 SHA256。

各版均包含源码 ZIP、逐文件清单、SHA256，以及可直接烧录的 `images/` 和 `firmware-版本-flash.zip`。烧录步骤见 [固件 README](../README.md#直接烧录发布固件无需编译)。不包含工具链或依赖缓存。

发布流程：更新 `CMakeLists.txt`、CHANGELOG、修复记录与 README；原生回归和 ESP-IDF 构建通过后暂存源码，执行 `python tools/archive-source.py 版本号`。再执行 `python tools/package-images.py 版本号` 从同版构建生成镜像与烧录 ZIP，校验文件版本、分区及 SHA256。两个脚本不覆盖已有归档。最后单独提交 release，并创建版本标签；未执行硬件验收时必须记录为待验收。
