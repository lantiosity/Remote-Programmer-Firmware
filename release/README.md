# 固件版本归档

当前修复版：**[0.1.2/](0.1.2/)**；历史首版：[0.1.0/](0.1.0/)，保留原归档。

各版均包含源码 ZIP、逐文件清单、SHA256，以及可直接烧录的 `images/` 和 `firmware-版本-flash.zip`。烧录步骤见 [固件 README](../README.md#直接烧录发布固件无需编译)。不包含工具链或依赖缓存。

发布流程：更新 `CMakeLists.txt`、CHANGELOG、修复记录与 README；原生回归和 ESP-IDF 构建通过后暂存源码，执行 `python tools/archive-source.py 版本号`。再执行 `python tools/package-images.py 版本号` 从同版构建生成镜像与烧录 ZIP，校验文件版本、分区及 SHA256。两个脚本不覆盖已有归档。最后单独提交 release，并创建版本标签；未执行硬件验收时必须记录为待验收。
