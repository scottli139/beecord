# Beecord 固件

录音手表固件，ESP-IDF 工程（目标芯片 ESP32-S3）。

## 构建前提

- [ESP-IDF](https://github.com/espressif/esp-idf) **v5.4.x**（工具链随 IDF 安装自带）
- 首次构建需网络：组件管理器会自动从 [ESP-IDF 组件仓库](https://components.espressif.com/)
  拉取开发板 BSP（`waveshare/esp32_s3_touch_amoled_2_06`）与 `esp_codec_dev`

## 构建

```bash
. ~/esp/esp-idf/export.sh        # 按你的 IDF 安装路径调整
cd firmware
idf.py set-target esp32s3
idf.py build
```

## 烧录

```bash
idf.py -p /dev/tty.usbserial-XXXX flash monitor    # 串口设备名按实际调整
```

烧录失败需进强制下载模式时：完全断电 → 按住 BOOT 上电 → 重新执行 flash。

## 目录结构

| 路径 | 说明 |
|---|---|
| `main/` | 应用入口（`app_main`）与各子系统代码 |
| `main/proto/` | BuzzLink 协议帧编解码（硬件无关纯 C，可宿主单测） |
| `main/audio/` | 音频采集 / 编码封装（预留） |
| `main/storage/` | TF 卡存储（预留） |
| `main/ble/` | BLE 与 BuzzLink 服务（预留） |
| `main/ui/` | 屏幕交互（预留） |
| `test_host/` | macOS / Linux 宿主单测，无需开发板 |

> M0 骨架阶段：仅工程链路可用，硬件功能随里程碑推进逐块落地。
