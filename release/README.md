# 固件版本归档

首版：[`0.1.0/`](0.1.0/)。包含原始源码 ZIP，以及可直接烧录的 [`images/`](0.1.0/images/) 和 `firmware-0.1.0-flash.zip`。烧录方法见 [固件 README](../README.md#直接烧录发布固件无需编译) 和 [镜像说明](0.1.0/images/README.md)。不放构建环境或缓存。

在项目根目录完成版本修改、构建验证并暂存源码后，执行 `python tools/archive-source.py 版本号`，再将该版本目录加入提交。脚本不会覆盖已存在的版本目录。归档不包含 `release/` 自身，避免嵌套历史包。

上述脚本仅归档源码；可烧录包还需从同版本成功构建的 `build/flasher_args.json` 取齐 bootloader、分区表、应用和初始 otadata，保留相对路径及 `flash_args`，并核对镜像版本、分区布局及 SHA256。发布前同时更新烧录包及其独立清单；应用镜像可供 OTA 使用。
