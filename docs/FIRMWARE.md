# Beecord 固件

录音手表固件：ESP32-S3 上采集编码、TF 卡存储、经 BuzzLink 协议（BLE）同步到电脑。
M0（骨架/协议层/宿主单测）、M1（到货自检：Flash 实测 32MB、自定义分区表、USB 控制台、
SDMMC 双卡实测）、M2（音频采集：ES7210 → 混单声道 → WAV 写 TF 卡 + 串口 REPL 录控，
上板验收通过）已完成；BLE 服务（M4）与表盘 UI（M5）等随后续里程碑落地。

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
│   ├── main.c                    # 启动入口：版本日志 → SD 探测 → 音频初始化 → 采集器 → REPL
│   ├── console_cmd.c             # USB 串口 REPL（esp_console）：rec/ls/get/gain 命令
│   ├── proto/                    # BuzzLink 协议层：硬件无关纯 C，宿主可测
│   │   ├── buzz_proto.h/.c       #   帧格式常量 + 编解码 + 字节流解析
│   │   ├── buzz_reasm.h/.c       #   FRAG 分片重组
│   │   └── crc16.h/.c            #   CRC-16/CCITT-FALSE（帧 CRC 与文件 CRC 共用）
│   ├── audio/
│   │   ├── ogg_writer.h/.c       # Ogg 页封装（RFC 3533，硬件无关）；Opus 编码 M3 接入
│   │   ├── buzz_audio.h/.c       # ES7210 采集封装：BSP 初始化 + 16k/16bit/双声道 + 33dB 增益
│   │   ├── wav_writer.h/.c       # WAV 封装（纯 stdio 硬件无关，宿主可测）：占位头 + 收尾回填
│   │   └── buzz_recorder.h/.c    # 采集任务（prio5/栈6K/4KB 缓冲）：读→混单声道→写 WAV
│   ├── storage/                  # buzz_sdcard：TF 卡挂载/探测（M1 实测 16GB/128GB 通过）
│   ├── ble/                      # 占位（M4：BuzzLink GATT 服务）
│   └── ui/                       # 占位（M5：表盘与交互）
└── test_host/                    # 宿主单测（详见下节）
```

## 宿主单测（无需开发板）

```bash
make -C firmware/test_host test          # 本机 clang；CI 用 `make CC=gcc`（ubuntu）
```

- **46 用例 / 706 断言**：帧编解码（§11 向量 V1~V4 往返）、字节流重组（重同步/容错）、
  分片重组（乱序/丢片/超上限）、CRC16、Ogg 页结构 + 每页 CRC32、Opus 头包、
  WAV 头字段逐字节（小端序/回填长度/落盘复读/空文件/API 守卫）
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
| 音频抽象 | `espressif/esp_codec_dev` ~1.5 | ✅ M2 已用（ES7210 采集，解析 1.5.11） |
| UI | LVGL 9.6（经 `esp_lvgl_port` 2.9.0 传递引入） | ✅ 依赖锁定，M5 起使用 |
| Ogg 封装 | 手写 `ogg_writer`（本仓 `main/audio/`，约 300 行） | ✅ 已实现 + 宿主单测 |
| 音频编码 | Opus（esp-opus / libopus），16kHz 单声道 | 🔜 M3 引入 |
| 存储 | FATFS + SDMMC 1-bit（TF 卡，BSP 实现） | ✅ M2 已用（录音写 WAV；M1 验证 16GB/128GB 挂载） |
| BLE | NimBLE（MTU 247） | 🔜 M4 引入（IDF 内置组件） |

## 里程碑路线图

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M0 环境与骨架** | 工程骨架 + 协议层 + Ogg 封装 + 宿主单测 + CI | ✅ 已完成（2026-10-18） |
| **M1 到货自检** | `esptool.py flash_id` 实测 Flash（32MB）并定分区表；出厂固件自检；骨架刷机 + USB 控制台启动验证 | ✅ 已完成（2026-10-19） |
| **M2 音频采集** | BSP + esp_codec_dev 初始化 ES7210，录 PCM/WAV 到 TF 卡 | ✅ 已完成（2026-10-19）：Mac 播放正常、30min soak 无错、宿主单测绿 |
| **M3 编码封装** | Opus 编码接 `ogg_writer` 写卡 | 播放器放 `.ogg`，时长/波形正确、页 CRC 全对 |
| **M4 BuzzLink BLE 服务** | NimBLE GATT + 全命令 + 下载流 | Beecord Studio 列文件/查信息/下载/CRC 验收通过 |
| **M5 端到端联调** | 录音→同步→Studio 全链路 + 表盘 UI | 一场真实录音完整同步回 Mac |
| **M6 打磨** | 电源管理、吞吐与稳定性调优 | 视情况 |

## 架构详述（占位）

以下内容随 M2~M5 实现后以实际代码为准补写，避免文档超前于实现：

### 录音管线（采集 → Opus → Ogg → TF 卡）

**M2 已落地段**：双麦 → ES7210（I2S Slave，16kHz/16bit/立体声）→ 软件混单声道 → WAV 写 `/sdcard`。
Opus 编码与 Ogg 封装随 M3 接入（`ogg_writer` 已就绪）。

- **初始化**（`buzz_audio`）：`bsp_audio_codec_microphone_init()`（BSP 2.0.0 内部幂等完成
  I2C/I2S）→ `esp_codec_dev_open(16k/16bit/2ch)` → `esp_codec_dev_set_in_gain(33.0)`。
  增益为上板扫频定案：24dB 电平偏低、36dB 近讲削顶，33dB 远近兼顾（0-37.5dB 可调）。
- **采集**（`buzz_recorder`，FreeRTOS 任务 prio 5 / 栈 6KB / 4KB 读缓冲 = 64ms 音频）：
  `esp_codec_dev_read`（内部 1000ms 超时，只回状态码）→ L/R 交错 int16 混单声道
  `(int32)(l+r)/2` 就地写回 → 追加写 WAV。非 OK 读取计数、连续 10 次失败中止上报。
  单声道码率 32KB/s，SDMMC 直写带宽余量充足。
- **WAV 封装**（`wav_writer`，纯 stdio 硬件无关）：open 写 44 字节占位头，close 时
  fseek 回填 RIFF/data 长度；文件名 `REC_%04d.WAV` 递增（启动与开录前双扫描取最大序号+1）。
- **录控**（`console_cmd`，USB Serial/JTAG 串口 REPL，提示符 `beecord> `）：
  `rec start/stop`、`ls`（含卡容量）、`get <文件> [字节数]`（base64 串口导出，免读卡器取文件，
  实测 ~27KB/s）、`gain <dB>`（运行期调增益）。REPL 在 M3/M4 继续复用。
- **握手协议**：start 在会话开始（文件打开后）即回应答，stop 收会话收尾应答（含统计进日志）；
  start 前清空应答队列防陈旧 ack。REPL 侧先做状态预检给出中文提示。
- **M2.4 实测修正**：①REPL 创建前不能再调 `esp_console_init`（`esp_console_new_repl_*` 内部
  已含，重复初始化报 `ESP_ERR_INVALID_STATE`）；②串口脚本须 `dtr=False` 并等待提示符同步，
  否则开端口触发的复位会吞掉命令。

### BuzzLink BLE 服务实现

*随 M4 落地后补写。*

### 电源管理

*随 M5~M6 落地后补写。*
