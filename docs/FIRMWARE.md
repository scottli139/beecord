# Beecord 固件

录音手表固件：ESP32-S3 上采集编码、TF 卡存储、经 BuzzLink 协议（BLE）同步到电脑。
M0（骨架/协议层/宿主单测）、M1（到货自检：Flash 实测 32MB、自定义分区表、USB 控制台、
SDMMC 双卡实测）、M2（音频采集：ES7210 → 混单声道 → WAV 写 TF 卡 + 串口 REPL 录控）、
M3（编码封装：Opus 定点编码 → Ogg 写卡，8MB PSRAM 启用）、M4（BuzzLink BLE 服务：
NimBLE GATT + 全命令 + 下载流，Beecord Studio 端到端验收通过）已完成。M5 已落地
表盘与录音 UI、按键/提示音、现场保护、设置与同步标记持久化、电源/RTC 和自动对时；
四页数据接真/格式化（M5.5）与端到端收官（M5.6）随后续里程碑完成。

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
│   ├── main.c                    # 启动入口：NVS/设置 → SD/音频 → RTC → BLE → UI/电源 → REPL
│   ├── fw_version.h              # FW_VERSION 单点定义（main 与 INFO 应答共用）
│   ├── console_cmd.c             # USB REPL：rec/ls/get/gain/quality/settings/pwr/rtc/ble 等
│   ├── proto/                    # BuzzLink 协议层：硬件无关纯 C，宿主可测
│   │   ├── buzz_proto.h/.c       #   帧格式常量 + 编解码 + 字节流解析
│   │   ├── buzz_reasm.h/.c       #   FRAG 分片重组
│   │   └── crc16.h/.c            #   CRC-16/CCITT-FALSE（帧 CRC 与文件 CRC 共用）
│   ├── audio/                    # ES7210 采集、Opus/Ogg/WAV、录音控制、电平与断电 journal
│   ├── storage/                  # TF 卡挂载 + file_index 枚举/分页/元数据/流式 CRC
│   ├── sys/                      # 按键/提示音、settings+sync_marks（NVS）、AXP2101、PCF85063
│   ├── ble/                      # NimBLE/GATT、协议服务、控制命令（含 TIME_SET）、下载推流
│   └── ui/                       # LVGL 表盘/录音页/浮层、主题字体、熄屏与快照测试
└── test_host/                    # 宿主单测（详见下节）
```

## 宿主单测（无需开发板）

```bash
make -C firmware/test_host test          # 本机 clang；CI 用 `make CC=gcc`（ubuntu）
```

- 覆盖帧编解码与协议向量、字节流/分片重组、CRC16、Ogg/Opus/WAV、rec_pipe 真编解码
  roundtrip、buzz_server、file_index；M5 新增 keyd/beep、rec_meter/journal、settings/
  sync_marks（值域/损坏数据/写失败/LRU）、power（平滑/迟滞/阈值）与 rtc（BCD/闰年/
  epoch/时区/本地窗口）硬件无关用例，另有全量 UI 字体字符门禁。
- 本机除默认 clang 外，可用 `make -C firmware/test_host CC=gcc-16 test` 跑真 GCC；
  macOS 名为 `gcc` 的命令通常是 Apple clang 别名，不能当作 GCC 回归。
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

## 里程碑补充（M5.4，2026-10-30）

- settings NVS：质量/增益/提示音/熄屏时间启动应用并持久化，亮度仅存值待背光方案；非法值回默认。已同步标记在 DL_END 成功后写入，256 项 LRU，重启保留。
- AXP2101 1Hz 采样：EMA 显示平滑、真实电量/充电态上表盘和 INFO；未充电且原始样本连续 3 次 <10% 时停止活动录音并反馈；PWR 4.5s 显示关机警告层。
- 协议 v0.2：TIME_SET（0x42/0x43，`epochMs`+`utcOffsetMin` → 本地日历写 RTC，frame version 不变）；PCF85063 驱动（OS 标志防御、写后读回校验、失败尽力恢复起振）；REPL `rtc` / `rtc set`；`tools/buzzlink_cli.py time` 独立对时验收。
- RTC 初始化在 BLE 之前（客户端连接窗口内 TIME_SET 无竞态）；表盘真实时钟（1Hz 时钟服务 + HH:MM/基线秒/月日周几；未对时显 "--:-- --"+"未对时"）。
- Studio：连接 INFO 后、LIST 前自动对时（失败降级不阻断同步）。

## 架构详述

以下按已落地实现记录；未完成的 M5.5/M5.6 能力不作超前承诺：

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
- **现场保护（M5.3）**：录音中每 5s flush 并写 journal，异常断电后开机检查并修复可恢复的
  未闭合文件；首个 journal 落盘前断电仍可能留下 0 字节孤儿文件，其清理/列表过滤归 M5.5。

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
  录音中 DL_BEGIN 回 busy；空闲 60s 主动断开（推流期间按出向进度复位计时）；
  0 字节文件下载合法（totalChunks=0，CRC=0xFFFF）。M5.4 起 INFO 电量取真实 AXP 快照，
  仅未知时 battery=-1；DL_END 完整结束后写入持久化已同步标记。
- **FILE_DELETE**：v1 未实现，应答 ERROR 帧 `{"code":4003,"message":"delete not supported in v1"}`。
- **已知客户端兼容注记**：macOS CoreBluetooth 偶发丢失订阅后首帧 write-without-response
  （~1/4 概率）；`tools/buzzlink_cli.py` 以 `canSendWriteWithoutResponse` 轮询 + PING 握手
  重试规避。

### 电源与 RTC

- `power_axp` 复用 BSP I2C，以 1Hz 读取 AXP2101 电量百分比、电池电压和充放电态；
  EMA(α=0.2) 只用于显示。低电判定使用原始样本，严格 <10% 连续 3 次触发、≥13%
  连续 5 次解除，充电中不触发；确认低电后通过录音控制任务显式停止活动会话。
- `rtc_pcf` 复用同一总线，PCF85063 存本地日历。TIME_SET 将 UTC epoch 与 Mac 当前
  UTC 偏移换算后写入，STOP 后任何失败都尽力恢复起振，成功起振再读回对拍（±1s）。
  独立 1Hz 时钟任务维护快照，LVGL 只读快照，避免在渲染任务里阻塞 I2C。
