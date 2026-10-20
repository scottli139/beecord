# Beecord 固件

录音手表固件：ESP32-S3 上采集编码、TF 卡存储、经 BuzzLink 协议（BLE）同步到电脑。
M0（骨架/协议层/宿主单测）、M1（到货自检：Flash 实测 32MB、自定义分区表、USB 控制台、
SDMMC 双卡实测）、M2（音频采集：ES7210 → 混单声道 → WAV 写 TF 卡 + 串口 REPL 录控）、
M3（编码封装：Opus 定点编码 → Ogg 写卡，8MB PSRAM 启用）、M4（BuzzLink BLE 服务：
NimBLE GATT + 全命令 + 下载流，Beecord Studio 端到端验收通过）已完成；
表盘 UI（M5）等随后续里程碑落地。

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
│   ├── main.c                    # 启动入口：版本日志 → NVS → SD 探测 → 音频初始化 → 采集器 → BLE → REPL
│   ├── fw_version.h              # FW_VERSION 单点定义（main 与 INFO 应答共用）
│   ├── console_cmd.c             # USB 串口 REPL（esp_console）：rec/ls/get/gain/ble 命令
│   ├── proto/                    # BuzzLink 协议层：硬件无关纯 C，宿主可测
│   │   ├── buzz_proto.h/.c       #   帧格式常量 + 编解码 + 字节流解析
│   │   ├── buzz_reasm.h/.c       #   FRAG 分片重组
│   │   └── crc16.h/.c            #   CRC-16/CCITT-FALSE（帧 CRC 与文件 CRC 共用）
│   ├── audio/
│   │   ├── ogg_writer.h/.c       # Ogg 页封装（RFC 3533，硬件无关）+ OpusHead/Tags 头包生成
│   │   ├── opus_enc.h/.c         # Opus 薄包装（opaque 句柄）：目标链 micro-opus，宿主链系统 libopus
│   │   ├── rec_pipe.h/.c         # 录音管线（硬件无关）：帧切片 320 采样 → Opus → Ogg 会话 → 页回调
│   │   ├── buzz_audio.h/.c       # ES7210 采集封装：BSP 初始化 + 16k/16bit/双声道 + 33dB 增益
│   │   ├── wav_writer.h/.c       # WAV 封装（纯 stdio 硬件无关，宿主可测）：占位头 + 收尾回填
│   │   └── buzz_recorder.h/.c    # 采集任务（prio5/栈8K/4KB 缓冲）：读→混单声道→Ogg/WAV 双格式写卡
│   ├── storage/                  # buzz_sdcard：TF 卡挂载/探测（M1 实测 16GB/128GB 通过）；
│   │                             # file_index：录音枚举/分页/元数据/流式 CRC（硬件无关，宿主可测）
│   ├── ble/                      # BuzzLink BLE 服务（M4，详见下文"BLE 服务实现"）
│   │   ├── buzz_ble.h/.c         #   NimBLE 初始化/广播（BCD-XXXX）/GAP 事件/连接参数/状态查询
│   │   ├── buzz_gatt.h/.c        #   GATT 服务表（三 UUID）/写回调/notify 发送
│   │   ├── buzz_server.h/.c      #   协议服务层（硬件无关，宿主可测）：分发/分片/ERROR/极简 JSON
│   │   ├── buzz_cmds.h/.c        #   胶合层：服务任务/队列/控制通道命令/空闲 60s 断开
│   │   └── buzz_dl.h/.c          #   下载状态机 + 推流泵（流控）
│   └── ui/                       # 占位（M5：表盘与交互）
└── test_host/                    # 宿主单测（详见下节）
```

## 宿主单测（无需开发板）

```bash
make -C firmware/test_host test          # 本机 clang；CI 用 `make CC=gcc`（ubuntu）
```

- **98 用例 / 1752 断言**：帧编解码（§11 向量 V1~V4 往返）、字节流重组（重同步/容错）、
  分片重组（乱序/丢片/超上限）、CRC16、Ogg 页结构 + 每页 CRC32、Opus 头包、
  WAV 头字段逐字节（小端序/回填长度/落盘复读/空文件/API 守卫）、
  **rec_pipe 全链路 roundtrip**（真 libopus 编解码：granule 序列/头页独占/EOS/尾帧丢弃/静音能量隔离）、
  **buzz_server 服务层**（§11 向量行为级比对/响应分片/重组请求/未知帧静默/ERROR 封装/
  会话回显/JSON 助手，25 用例）、**file_index**（内存假文件系统注入：枚举排序/分页边界/
  双格式 durationMs/流式 CRC/CRC 缓存行为/0 字节与损坏头容错，23 用例）
- **会话向量回放**：与 Mac 侧 XCTest 共享同一份 JSON（`buzzlink-vector/1` schema），
  由 `tools/vector2c.py` 生成 C 表对拍——机制见 [TESTING.md](TESTING.md) §3
- **Ogg 对拍**：测试产出的 `.ogg` 由独立校验器 `tools/ogg_check.py` 逐页复核
  CRC32（含坏页反向验证）
- roundtrip 用例依赖系统 libopus（本机 `brew install opus`；CI 装 `libopus-dev`），
  无库时跳过并打印提示；CI 必装不允许静默跳过
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
| 音频编码 | `esphome/micro-opus` ^0.3.6（定点模式，16kHz 单声道 VBR ~44kbps） | ✅ M3 已用（解析 0.3.6） |
| PSRAM | 8MB octal（80MHz，caps_alloc） | ✅ M3 启用（Opus state/pseudostack 落 PSRAM） |
| 存储 | FATFS + SDMMC 1-bit（TF 卡，BSP 实现） | ✅ M2 已用（录音写 Ogg/WAV；M1 验证 16GB/128GB 挂载） |
| BLE | NimBLE（MTU 247，IDF 内置组件） | ✅ M4 已用（GATT 服务 + 全命令 + 下载流） |

## 里程碑路线图

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M0 环境与骨架** | 工程骨架 + 协议层 + Ogg 封装 + 宿主单测 + CI | ✅ 已完成（2026-10-18） |
| **M1 到货自检** | `esptool.py flash_id` 实测 Flash（32MB）并定分区表；出厂固件自检；骨架刷机 + USB 控制台启动验证 | ✅ 已完成（2026-10-19） |
| **M2 音频采集** | BSP + esp_codec_dev 初始化 ES7210，录 PCM/WAV 到 TF 卡 | ✅ 已完成（2026-10-19）：Mac 播放正常、30min soak 无错、宿主单测绿 |
| **M3 编码封装** | Opus 定点编码（micro-opus）+ PSRAM 启用 + `rec_pipe` 管线接 `ogg_writer` 写卡 | ✅ 已完成（2026-10-19）：ffplay 播放正常、时长精确、页 CRC 双对拍全过、30min soak 无错、WAV 回归通过 |
| **M4 BuzzLink BLE 服务** | NimBLE GATT + 全命令 + 下载流 | ✅ 已完成（2026-10-20）：Studio 列文件/查信息/下载/CRC 验收全通过（含 9.7MB 全量落库），实测吞吐 42~54KB/s |
| **M5 端到端联调** | 录音→同步→Studio 全链路 + 表盘 UI | 一场真实录音完整同步回 Mac |
| **M6 打磨** | 电源管理、吞吐与稳定性调优 | 视情况 |

## 架构详述（占位）

以下内容随 M2~M5 实现后以实际代码为准补写，避免文档超前于实现：

### 录音管线（采集 → Opus → Ogg → TF 卡）

**M3 全链路已落地**：双麦 → ES7210（I2S Slave，16kHz/16bit/立体声）→ 软件混单声道 →
`rec_pipe`（帧切片 → Opus 定点编码 → `ogg_writer` Ogg 封装）→ 页回调 `fwrite` 写 `/sdcard`。
WAV 格式保留并存（`rec start wav`）。

- **初始化**（`buzz_audio`）：`bsp_audio_codec_microphone_init()`（BSP 2.0.0 内部幂等完成
  I2C/I2S）→ `esp_codec_dev_open(16k/16bit/2ch)` → `esp_codec_dev_set_in_gain(33.0)`。
  增益为上板扫频定案：24dB 电平偏低、36dB 近讲削顶，33dB 远近兼顾（0-37.5dB 可调）。
- **采集**（`buzz_recorder`，FreeRTOS 任务 prio 5 / 栈 8KB / 4KB 读缓冲 = 64ms 音频）：
  `esp_codec_dev_read`（内部 1000ms 超时，只回状态码）→ L/R 交错 int16 混单声道
  `(int32)(l+r)/2` 就地写回 → 按格式分发到 `rec_pipe`（Ogg）或 `wav_writer`（WAV）。
  非 OK 读取计数、连续 10 次失败中止上报。
- **Opus 编码**（`opus_enc` 薄包装，opaque 句柄）：`opus_encoder_create(16000, 1,
  OPUS_APPLICATION_AUDIO)` + 44kbps VBR + complexity 5。micro-opus 组件以**定点模式**构建
  （`CONFIG_OPUS_FLOATING_POINT=n`：S3 上浮点 SILK 编码慢 4~6 倍无法实时，官方 benchmark 结论）；
  编码器 state 与 120KB pseudostack 经组件默认配置落 PSRAM（M3 起启用 8MB octal PSRAM）。
  目标侧链 micro-opus、宿主侧链系统 libopus，同码两编故整条管线可宿主 roundtrip。
- **Ogg 会话**（`rec_pipe`，硬件无关零 malloc）：init 写 RFC 7845 头两页（OpusHead BOS 独占
  首页、OpusTags 各占一页）；feed 跨读块攒 320 采样（20ms）帧切片后编码，
  granule 48kHz 域首包 960、逐帧 +960，preskip=312 仅头声明；页缓冲 4KB（约 28 包/页），
  页满经回调写卡（码率 ~5.6KB/s，写频 ~1.8 次/s，远低于 WAV 的 8 次/s）；finish 丢弃不足
  一帧的尾采样（<20ms），末包带 EOS flag 封页（空会话则产空 EOS 页）。
  单任务直写沿用 M2 结论：编码耗时（定点 SILK 16kHz 单声道）与写页阻塞远小于 64ms 读节奏。
- **双格式与文件序号**：`buzz_recorder_start(buzz_rec_fmt_t)` 选 Ogg（默认）/WAV；
  `REC_%04d` 序号池两种后缀共用（扫目录取全局最大+1）。
- **录控**（`console_cmd`，USB Serial/JTAG 串口 REPL，提示符 `beecord> `）：
  `rec start [ogg|wav]` / `rec stop`、`ls`（含卡容量）、`get <文件> [字节数]`（base64 串口导出，
  免读卡器取文件，实测 ~46KB/s）、`gain <dB>`（运行期调增益）、`ble`（连接态/MTU/订阅态查询）。
- **握手协议**：start 在会话开始（文件打开后）即回应答，stop 收会话收尾应答（含统计进日志）；
  start 前清空应答队列防陈旧 ack。REPL 侧先做状态预检给出中文提示。
- **M2.4 实测修正**：①REPL 创建前不能再调 `esp_console_init`（`esp_console_new_repl_*` 内部
  已含，重复初始化报 `ESP_ERR_INVALID_STATE`）；②串口脚本须 `dtr=False` 并等待提示符同步，
  否则开端口触发的复位会吞掉命令。
- **M3.4 实测补充**：串口会话中途断开（host 侧 close/复位）会打断录音并留下不完整文件
  （无 EOS 页，极端情况 0 字节）——属已接受的"断电丢尾"范畴，周期 flush 与现场保护列 M6 候选。

### BuzzLink BLE 服务实现

**M4 全链路已落地**：NimBLE GATT 服务 + BUZZLINK.md 全命令 + 下载流，Beecord Studio
端到端验收通过（扫描/连接/INFO/LIST 翻页/FILE_INFO/下载三方 CRC/ABORT/断连恢复/
录音互斥）。协议唯一依据 [BUZZLINK.md](BUZZLINK.md)。

- **分层**：`buzz_ble`（NimBLE 初始化/广播/事件）→ `buzz_gatt`（服务表/写回调/notify）→
  `buzz_server`（**硬件无关**协议服务层：帧重组/分发/分片/ERROR 封装，宿主单测覆盖）→
  `buzz_cmds`（胶合：FreeRTOS 队列 + 服务任务 + 控制通道命令）→ `buzz_dl`（下载状态机 + 推流泵）。
- **线程模型**：GATT 写回调在 NimBLE host 任务内只把字节推入定长队列（8×260B，满丢弃）；
  buzz 服务任务（prio 4，低于采集任务 prio 5）是唯一 server 上下文，文件 I/O 全在此；
  notify 发送在服务任务内直接调用。断连即作废传输并复位 server 状态（协议 §7）。
- **广播与发现**：AD = flags + 128-bit Service UUID；Complete Local Name 入 scan response；
  设备名 `BCD-XXXX`（蓝牙 MAC 末 2 字节大写 hex）。单连接（`MAX_CONNECTIONS=1`）、
  无配对绑定、2M PHY preferred、连接后请求 interval 15~30ms/latency 0/timeout 2s。
- **MTU 自适应**：MTU 事件后 `set_max_payload(att_mtu-15)`——分片大小与
  `chunkDataLen = min(224, att_mtu-17)` 均随实际协商值（macOS 实测 247）。
- **推流流控（实测修正）**：`BLE_GAP_EVENT_NOTIFY_TX` 在本 NimBLE 移植于"入队尝试"时
  即触发（非发送完成），不能作信用信号；改为服务任务 2ms 滴答轮询泵——贪婪发至
  `BLE_HS_ENOMEM` 停发、控制器释放 mbuf 后下一 tick 续发。设备泵速与对端收速双端
  对拍一致，流控无损。
- **吞吐实测**：**42~54 KB/s**（macOS 中央）。瓶颈 = Apple 连接间隔下限 15ms × 每事件
  ~3 包 × 协议块 230B；mbuf 池（`STATIC_ACL_TX_BUF_NB=12`/`MSYS_1_BLOCK_COUNT=50`）、
  DLE、2M PHY、间隔请求均已调优，固件侧手段穷尽；iOS/ESP 中央预期更高。
- **SD 读速物理上限**：BSP 固定 SDMMC 1-bit 20MHz，实测 ~1MB/s。55MB 文件冷算 CRC
  ~80s，故 `file_index` 带 RAM CRC 缓存（32 项，key=id+size——录音文件写后不改）；
  产品路径（FILE_INFO 一次→DL_BEGIN 命中）稳态 ~47ms。持久化索引列 M6。
- **决策落地**（M4_BLE_PLAN D1~D10）：id = 文件名含扩展名（`REC_0014.OGG`）；
  INFO battery=-1（未知，电量计随 M5）；录音中 DL_BEGIN 回 busy；空闲 60s 主动断开
  （推流期间按出向进度复位计时）；0 字节文件下载合法（totalChunks=0，CRC=0xFFFF）。
- **FILE_DELETE**：v1 未实现，应答 ERROR 帧 `{"code":4003,"message":"delete not supported in v1"}`。
- **已知客户端兼容注记**：macOS CoreBluetooth 偶发丢失订阅后首帧 write-without-response
  （~1/4 概率）；`tools/buzzlink_cli.py` 以 `canSendWriteWithoutResponse` 轮询 + PING 握手
  重试规避。

### 电源管理

*随 M5~M6 落地后补写。*
