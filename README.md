# ESP32-S3 Badge Firmware

这是一个面向 Spotpear ESP32-S3 1.28 英寸圆屏盒子的电子徽章固件。它已经从最初的小智语音聊天固件演化成独立 badge firmware：启动显示壁纸，BOOT 键播放声音、切换壁纸、录音，素材和录音走 SD 卡。

当前目标硬件：

- 主控：ESP32-S3，16 MB flash，8 MB PSRAM
- 屏幕：1.28 英寸 240x240 GC9A01 圆屏，RGB565，SPI
- 音频：ES8311 codec，单声道录放
- 存储：microSD，SDMMC 1-bit
- 输入：BOOT 键，GPIO0

## 给用户

### 下载固件

到 GitHub Releases 下载最新的 `esp32-s3-badge-firmware-merged.bin`。这是推荐给普通用户使用的合并固件，直接从 `0x0` 写入即可。

仓库地址：

```text
https://github.com/wtxj00789d/esp32-s3-badge-firmware
```

### 刷机

安装 Python 和 Espressif 的 `esptool.py` 后，将设备通过 USB 连接到电脑。Windows 下串口通常类似 `COM6`，macOS/Linux 下通常类似 `/dev/ttyACM0` 或 `/dev/ttyUSB0`。

擦除 flash：

```bash
python -m esptool --chip esp32s3 --port COM6 erase_flash
```

写入合并固件：

```bash
python -m esptool --chip esp32s3 --port COM6 --baud 921600 write_flash 0x0 esp32-s3-badge-firmware-merged.bin
```

如果写入失败，先降低波特率：

```bash
python -m esptool --chip esp32s3 --port COM6 --baud 460800 write_flash 0x0 esp32-s3-badge-firmware-merged.bin
```

### SD 卡目录

第一次启动时，固件会尝试在 SD 卡上使用这些目录：

```text
/BADGE
/REC
```

壁纸和声音放在 `/BADGE`。录音会写到 `/REC`。

推荐文件命名：

```text
/BADGE/001.BWP
/BADGE/001.WAV
/BADGE/002.BWP
/BADGE/002.WAV
/REC/REC0001.WAV
```

`BWP` 是这个固件使用的壁纸格式。当前设计目标是 240x240、RGB565、最高 15 FPS。静态图片也可以作为 1 帧 BWP 使用。

同名 WAV 会作为当前壁纸的声音：

```text
001.BWP -> 001.WAV
```

WAV 建议使用：

```text
PCM
24000 Hz
16-bit
mono
```

### 按键操作

正常显示时：

```text
单击 BOOT    播放当前壁纸对应的 WAV
双击 BOOT    切换到下一张壁纸
长按 BOOT    开始录音
```

录音时：

```text
单击 BOOT    停止录音并返回壁纸
双击 BOOT    忽略
长按 BOOT    忽略
```

没有 SD 卡或 SD 卡没有有效素材时，固件会显示内置默认页面。没有同名 WAV 时，单击不会导致崩溃，只是没有声音可播。

## 给开发者

### 项目定位

这个仓库现在是专用 ESP32-S3 badge firmware，不再是完整的小智聊天应用。保留的重点是板级硬件初始化、LCD 直绘、ES8311 音频、SD 卡文件系统、NVS 设置和 badge 应用状态机。

当前 badge 应用入口：

```text
main/main.cc
main/badge/
```

核心模块：

```text
main/badge/badge_application.*   产品状态机
main/badge/badge_board.*         LCD、音频、按键、SD 相关板级封装
main/badge/badge_defaults.*      无 SD / 录音 / 错误等内置页面
main/badge/badge_storage.*       SD 挂载和素材扫描
main/badge/badge_settings.*      NVS 设置
main/badge/badge_bwp.*           BWP 壁纸加载和播放
main/badge/badge_sound.*         WAV 播放
main/badge/badge_recorder.*      WAV 录音
```

仍然复用的底层代码包括音频 codec、公共 button/backlight/I2C helper 等。旧的小智云端协议、聊天状态机、OTA、唤醒词、LVGL UI 等不属于这个 badge 固件的运行路径。

### 构建环境

目标 ESP-IDF：

```text
ESP-IDF 5.5.x
target: esp32s3
```

在 ESP-IDF PowerShell 或已激活 ESP-IDF 环境中：

```bash
idf.py set-target esp32s3
idf.py build
```

当前配置选择的板型：

```text
CONFIG_BOARD_TYPE_SPOTPEAR_ESP32_S3_1_28_BOX=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m.csv"
```

### 本地刷写开发版

开发时可以直接使用 ESP-IDF：

```bash
idf.py -p COM6 flash monitor
```

如果只想生成合并固件，可以在 build 后使用 ESP-IDF 生成的 merged binary，或按 release 中的合并固件发布方式导出。

### 固件产物说明

Release 中优先使用：

```text
esp32-s3-badge-firmware-merged.bin
```

这是完整 flash 镜像，适合普通用户从 `0x0` 写入。

工程调试时可能还会看到拆分镜像：

```text
flash_partition_table.bin
flash_otadata.bin
flash_ota0_header.bin
flash_ota1_header.bin
flash_app_0x20000.bin
```

这些用于理解分区和 OTA app 布局。普通用户不需要手动组合它们，除非正在调试启动、分区或 OTA 行为。

### 设计笔记

更多背景见：

```text
docs/superpowers/specs/2026-05-09-badge-firmware-design.md
docs/badge-firmware-notes.md
```

特别注意 GC9A01 的 RGB565 字节序问题：BWP 文件保持正常 RGB565，LCD 传输前做字节序适配，不要把错误字节序写进素材格式。
