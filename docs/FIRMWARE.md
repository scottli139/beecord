# Beecord 固件

录音手表固件：ESP32-S3 上采集编码、TF 卡存储、经 BuzzLink 协议（BLE）同步到电脑。
固件本体当前为 **M0 骨架阶段**——工程链路、协议层与 Ogg 封装已就绪并通过宿主单测，
硬件功能（音频/存储/BLE/UI）随 M2~M5 里程碑逐块落地；
M1（到货自检）已完成：Flash 实测 32MB、自定义分区表与 USB 控制台启用、烧录链路验证通过。

## 构建前提

- [ESP-IDF](https://github.com/espressif/esp-idf) **v5.4.x**（本机开发验证 5.4.4；CI 用 `espressif/idf:v5.4` 容器）
- 首次构建需网络：组件管理器自动从 [组件仓库](https://components.espressif.com/) 拉取
  BSP 与音频抽象层（版本锁定见 `main/idf_component.yml` + `dependencies.lock`）

## 构建与烧录

```bash
. ~/esp/esp-idf/export.sh        # 按你的 IDF 安装路径调整
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor    # USB-C 直连，端口名按实际调整
```

- 本板**无 USB-UART 桥**：USB-C 直连 ESP32-S3 原生 USB，控制台与烧录都走 **USB Serial/JTAG**
  （`sdkconfig.defaults` 已配 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`）。
- 本固件运行期保持 USB 枚举，esptool 可自动复位进下载模式，日常刷机**无需按键**。
- 若板子当前跑的是出厂固件（运行期关闭 USB、系统看不到设备），需先进强制下载模式：
  关机（PWR 长按 ~6s）→ **按住 BOOT（右上侧键）再插 USB 上电** → 松手后执行 flash。
  下载模式下屏幕黑屏属正常。

## Flash 与分区表

Flash 实测 **32MB**。自定义分区表 `partitions.csv`：

| 分区 | 类型 | 大小 | 用途 |
|---|---|---|---|
| nvs | data | 24KB | 配置/状态 |
| phy_init | data | 4KB | RF 校准 |
| factory | app | 4MB | 固件本体（不做 OTA） |
| coredump | data | 64KB | 崩溃转储（开发期调试） |
| storage | data(fat) | 1MB | 录音索引等少量持久数据（录音本体在 TF 卡） |

其余空间预留。

## 目录结构

```
firmware/
├── CMakeLists.txt / sdkconfig.defaults / partitions.csv / dependencies.lock
├── main/                         # 固件应用（idf.py build 的 main 组件）
│   ├── main.c                    # 启动入口（M0 仅打印版本日志）
│   ├── proto/                    # BuzzLink 协议层：硬件无关纯 C，宿主可测
│   │   ├── buzz_proto.h/.c       #   帧格式常量 + 编解码 + 字节流解析
│   │   ├── buzz_reasm.h/.c       #   FRAG 分片重组
│   │   └── crc16.h/.c            #   CRC-16/CCITT-FALSE（帧 CRC 与文件 CRC 共用）
│   ├── audio/
│   │   └── ogg_writer.h/.c       # Ogg 页封装（RFC 3533，硬件无关）；采集/编码随 M2/M3
│   ├── storage/                  # 占位（M2~M3：TF 卡存储）
│   ├── ble/                      # 占位（M4：BuzzLink GATT 服务）
│   └── ui/                       # 占位（M5：表盘与交互）
└── test_host/                    # 宿主单测（详见下节）
```

## 宿主单测（无需开发板）

```bash
make -C firmware/test_host test          # 本机 clang；CI 用 `make CC=gcc`（ubuntu）
```

- **42 用例 / 636 断言**：帧编解码（§11 向量 V1~V4 往返）、字节流重组（重同步/容错）、
  分片重组（乱序/丢片/超上限）、CRC16、Ogg 页结构 + 每页 CRC32、Opus 头包
- **会话向量回放**：与 Mac 侧 XCTest 共享同一份 JSON（`buzzlink-vector/1` schema），
  由 `tools/vector2c.py` 生成 C 表对拍——机制见 [TESTING.md](TESTING.md) §3
- **Ogg 对拍**：测试产出的 `.ogg` 由独立校验器 `tools/ogg_check.py` 逐页复核
  CRC32（含坏页反向验证）
- CI：`firmware-host-test`（ubuntu + gcc）与 `firmware-build`
  （`espressif/idf:v5.4` 容器编译回归）两个 job

## 技术栈与版本

| 模块 | 选型 | 状态 |
|---|---|---|
| 框架 | ESP-IDF v5.4.x | ✅ 已用（本机 5.4.4 / CI 容器 v5.4） |
| 板级支持 | `waveshare/esp32_s3_touch_amoled_2_06` ^2.0.0 | ✅ 依赖锁定（解析 2.0.0） |
| 音频抽象 | `espressif/esp_codec_dev` ~1.5 | ✅ 依赖锁定（解析 1.5.11），M2 起使用 |
| UI | LVGL 9.6（经 `esp_lvgl_port` 2.9.0 传递引入） | ✅ 依赖锁定，M5 起使用 |
| Ogg 封装 | 手写 `ogg_writer`（本仓 `main/audio/`，约 300 行） | ✅ 已实现 + 宿主单测 |
| 音频编码 | Opus（esp-opus / libopus），16kHz 单声道 | 🔜 M3 引入 |
| 存储 | FATFS + SDSPI（TF 卡） | 🔜 M2 引入（IDF 内置组件） |
| BLE | NimBLE（MTU 247） | 🔜 M4 引入（IDF 内置组件） |

## 里程碑路线图

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M0 环境与骨架** | 工程骨架 + 协议层 + Ogg 封装 + 宿主单测 + CI | ✅ 已完成（2026-10-18） |
| **M1 到货自检** | `esptool.py flash_id` 实测 Flash（32MB）并定分区表；出厂固件自检；骨架刷机 + USB 控制台启动验证 | ✅ 已完成（2026-10-19） |
| **M2 音频采集** | BSP + esp_codec_dev 初始化 ES7210，录 PCM/WAV 到 TF 卡 | Mac 播放 WAV 正常 |
| **M3 编码封装** | Opus 编码接 `ogg_writer` 写卡 | 播放器放 `.ogg`，时长/波形正确、页 CRC 全对 |
| **M4 BuzzLink BLE 服务** | NimBLE GATT + 全命令 + 下载流 | Beecord Studio 列文件/查信息/下载/CRC 验收通过 |
| **M5 端到端联调** | 录音→同步→Studio 全链路 + 表盘 UI | 一场真实录音完整同步回 Mac |
| **M6 打磨** | 电源管理、吞吐与稳定性调优 | 视情况 |

## 架构详述（占位）

以下内容随 M2~M5 实现后以实际代码为准补写，避免文档超前于实现：

### 录音管线（采集 → Opus → Ogg → TF 卡）

*随 M2~M3 落地后补写。*

### BuzzLink BLE 服务实现

*随 M4 落地后补写。*

### 电源管理

*随 M5~M6 落地后补写。*
