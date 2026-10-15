# Beecord Studio 技术设计（macOS）

> 状态：**v1.1**（2026-10-15：T0.5/T1/T2/T3 已落地，本文同步实施现状；下一步 T4/T5）。
> v1.0 于 2026-10-14 评审通过：总体形态与 PlayerEngine 决策按本方案执行；
> AI 预设 Kimi 默认模型改为 `kimi-k3`，见 §5.5。
> 上游依据：[`ux-design.md`](ux-design.md)（交互定稿）、
> [`DESIGN_SYSTEM.md`](DESIGN_SYSTEM.md)（令牌 + AX 规则 R1-R8）、[`BUZZLINK.md`](BUZZLINK.md)（协议规范）、
> [`BRAND.md`](BRAND.md)（品牌资产）。实施约束：[`CODING_STANDARDS.md`](CODING_STANDARDS.md)、
> [`TESTING.md`](TESTING.md)。
> 已决工程参数：部署目标 **macOS 15**；原生 `.xcodeproj` 入库；仓内签名为 Manual + ad-hoc
> （CI 用 `CODE_SIGNING_ALLOWED=NO`；本地真机调试用命令行覆盖个人证书，签名信息不入库）；
> Bundle ID `com.beeuc.beecord.studio`；Swift 6.3 / SwiftUI。

## 1. 目标与范围

| 阶段 | 内容 |
|---|---|
| **P0** | 扫描/连接/设备信息、文件列表/勾选下载/自动同步、CRC 校验、录音库管理、播放器（波形/倍速）、协议控制台、AX 可测（UITests 主流程） |
| **P1** | whisper.cpp 本地转写、AI 纪要（Kimi/DeepSeek/Qwen/自定义 OpenAI 兼容）、导出 m4a/wav、英文 i18n |
| 非目标 | iOS/iPadOS 版、iCloud 同步、实时转写、多设备并发连接、公证分发（notarization） |

关键事实：音频链 spike（原 P0 风险 S1）**已提前完成并验证**，结论见 §5.1——最大技术不确定性已消除。

## 2. 工程形态

```
apps/mac/
├── BeecordStudio.xcodeproj          # 原生工程，入库
├── BeecordStudio/                   # App target（SwiftUI）
│   ├── App/                         # 入口、NavigationSplitView、Settings 场景
│   ├── Features/{Device,Library,Console,Settings}/
│   ├── Services/{SyncEngine,PlayerEngine,ConsoleStore,AIService(P1),TranscribeEngine(P1)}/
│   └── Resources/Assets.xcassets    # 图标取自 docs/brand
├── BuzzLinkSim/                     # 虚拟设备 app target（macOS + iOS/iPadOS 双平台；BLE 外设
│                                    #   + loopback TCP 127.0.0.1:40404 同机通道，见 TESTING.md §4）
├── Packages/BuzzLink/               # 本地 SwiftPM
│   ├── Sources/BuzzLinkCore/        # 纯算法：帧/CRC/向量（无 CoreBluetooth 依赖）
│   ├── Sources/BuzzLinkSimCore/     # 设备侧协议引擎 SimEngine + 故障注入（纯逻辑，无 IO 依赖）
│   ├── Sources/BuzzLink/            # ✅ T2 已交付：Central 封装（BuzzLinkClient actor
│   │                                #   + BLE/loopback 双 Transport）
│   └── Tests/                       # 三个 test target（Core/SimCore/Central 共 136 用例）+ Fixtures
└── BeecordStudioUITests/            # AX 驱动 P0 主流程
```

- **零三方依赖（P0）**；P1 仅引入 whisper.cpp（官方 SwiftPM 源码集成）。
- 并发模型：BLE 回调 → `AsyncStream`；UI 层 `@Observable`；下载/转写任务结构化并发。
- Sandbox 开启：`com.apple.security.device.bluetooth`、`com.apple.security.network.client`、
  `files.user-selected.read-write`（导出/模型目录选择）。Sim 同样开 Sandbox。
- 分发：GitHub 源码 + 用户自行构建；README 写清构建步骤（ad-hoc 签名即可本地运行，
  无需开发者账号；CI 亦按无证书构建）。

## 3. 总体架构

![架构图](images/studio-arch.png)

依赖规则：**UI 不直接触碰 CoreBluetooth**，设备交互只经 `BuzzLink`（Central 层，
唯一含 CoreBluetooth/Network 依赖的 SDK target）；`BuzzLinkCore` 仅依赖 Foundation（可在 Linux 跑单测）；
`BuzzLinkSim` 复用 `BuzzLinkCore` 帧层实现设备侧，与 Studio 互为对端——开发、联调、UITests 都不依赖硬件。

## 4. BuzzLink SDK

### 4.1 BuzzLinkCore（纯算法）

- 帧编解码：`BE EC` 魔数帧（type/session/channel/length/payload/crc16），含 notify 流粘包重组；
- CRC-16/CCITT-FALSE（查表法）；
- 测试向量 JSON 的 Codable 模型——**与协议控制台导出格式同一份 schema**（ux-design §3.5），
  控制台导出的会话可直接作为 `Tests/Fixtures` 回放。

### 4.2 BuzzLink（Central）

✅ **T2 已交付**（2026-10-15）。实际形态：

- `BuzzLinkClient`（actor）：`connect/disconnect`、`ping`、`getDeviceInfo`、
  `listFiles(offset:limit:)`（分页）、`fileInfo(id:)`、整文件 `download(fileID:progress:)`
  （内置 BEGIN/CHUNK/END 全流程与重试；默认命令超时 5s、块超时 10s、3 趟，均可注入）；
  状态流 `stateUpdates`：`idle → scanning → connecting → ready → downloading → verifying → ready`，
  断连收敛到 `disconnected`（自动重连策略在 SyncEngine 层决定）；
- `BuzzLinkTransport` 抽象：`LoopbackTransport`（NWConnection → 127.0.0.1:40404，同机主通道）
  + `BLETransport`（CBCentralManager 薄壳：扫描 → 连接 → 服务/特征发现 → MTU → ready，
  write-without-response + 背压；**仅编译验证**，跨机实测见 TESTING.md §4 缺口条目）；
- **协议常量集中 `BuzzLinkConstants` 一处**，以 [`BUZZLINK.md`](BUZZLINK.md) 规范为准。

扩展点（未做，按需再补）：beginDownload/chunk 流/endVerify 细粒度 API（T7 控制台逐帧展示用）；
即时取消（现取消不中断在飞命令，由超时 ≤10s 兜底回收，T5 需要再补）；删除等写命令 P1 评估。

### 4.3 可靠性策略（P0）

设备推流 + 块序号 + 整文件 CRC16 验收，失败整文件重下（T2 实现默认 3 趟，
含块/命令超时后的 DL_ABORT 重试）；块位图选择性重传留作协议演进点（P1+ 评估）。

### 4.4 测试

✅ 现状 136 用例：Core 帧/CRC/向量 + Sim 引擎（含故障注入矩阵）+ Central（命令/下载/状态机、
乱序/截断/逐字节重组、进程内 SimEngine 集成——真实 Ogg 样本全量下载验收、
`LoopbackLiveSmokeTests` 活体冒烟：Sim 未运行时自动 skip）。全部 XCTest、不依赖蓝牙硬件；
CI 门禁 `mac-test`（GitHub Actions，macos-26 + Xcode 26.6）执行本条与 xcodebuild 两条命令，
首跑已通过（2026-10-15）。

## 5. Studio app 服务层

### 5.1 PlayerEngine（关键决策）

**Spike 结论（2026-10-14 已实测，本机 macOS 26.5 SDK）**：

| 验证项 | 结果 |
|---|---|
| Ogg 页解封装（纯 Swift） | ✅ 可行（约 40 行） |
| CoreAudio **Opus 解码器**（16k/48kHz） | ✅ 存在；解码输出与 libopus 参考对齐 **93.4 dB SNR**，幅值一致 |
| CoreAudio Opus **编码器** | ❌ 不存在（不需要：设备端已编码，导出用 AAC/LPCM） |
| ffmpeg CAF muxer 写 opus | ❌ 不支持 |
| 手写 CAF-opus（AudioFileWritePackets） | ❌ 第二包报 `'pty?'`（系统行为不稳定，放弃） |

**决策**：
1. **导入时一次转码** `ogg → m4a（AAC 48kbps）` 存库；播放、波形、导出、分享全部走 m4a
   （AVAudioFile / AVAssetReader，系统全场景兼容）；
2. 原 `.ogg` 保留在库目录，供"导出原始文件"；
3. 转码链：自研 Ogg demuxer → AudioConverter 流式解码 → AVAssetWriter（AAC）；
4. 播放：AVAudioEngine + AVAudioPlayerNode + AVAudioUnitTimePitch（1/1.5/2 倍速）；
   波形：AVAssetReader 分块取 min/max（长录音不整段驻留内存）；
5. 导出：`m4a`（直拷）、`wav`（解码 PCM 写入）、`ogg`（原件）。

弃用方案留档：CAF-opus 直读（系统支持不完整）；捆绑 ffmpeg（体积与签名复杂度高）。

### 5.2 SyncEngine

- 自动同步**默认开启**（ux 已决）：连接 ready → 拉取设备列表 → 与库 diff → 队列化下载
  → CRC 校验 → 转码入库 → 清理 `.partial`；
- 中断恢复：`.partial` 文件存在即未完成，重连后重下该文件；按设备文件 id 幂等，重复同步不产生副本；
- 进度模型：`SyncProgress { current, total, fileFraction, bytesPerSecond, eta }` 供 UI/AX 断言。

### 5.3 录音库模型

**文件系统即数据库**（本地优先，Finder 可直接浏览）：

```
Application Support/Beecord/
├── Library/<recording-id>/
│   ├── audio.ogg            # 原始文件
│   ├── audio.m4a            # 播放用转码产物
│   ├── meta.json            # 标题/标签/时长/大小/CRC/来源设备/转写·纪要状态
│   ├── transcript.json      # P1，分段+时间戳
│   └── summary.md           # P1
├── Library/.partial/        # 下载中
└── models/whisper/          # P1，ggml 模型
```

`recording-id` = 设备文件 id（起录时间），冲突加短后缀。启动时目录扫描对账 + manifest 缓存加速。

### 5.4 ConsoleStore（协议控制台）

环形缓冲（最近 5000 帧）；方向/type 过滤；行展开 hex；**导出 JSON 测试向量**
（schema 与 BuzzLinkCore 共用，见 §4.1）。

### 5.5 AIService（P1）

OpenAI 兼容 `POST /chat/completions`（SSE 流式，`URLSession.bytes`）；预设表：

| 预设 | baseURL | 默认模型 |
|---|---|---|
| Kimi | `https://api.moonshot.cn/v1` | `kimi-k3` |
| DeepSeek | `https://api.deepseek.com/v1` | `deepseek-chat` |
| Qwen | `https://dashscope.aliyuncs.com/compatible-mode/v1` | `qwen-plus` |
| 自定义 | 用户填写 | 用户填写 |

API key 存 Keychain（`kSecAttrAccessibleAfterFirstUnlock` + `ThisDeviceOnly`）；设置页"测试连接"；
首次开启 AI 功能时按 ux §1 弹出数据出域提示。模型名以设置页可编辑文本为准，上表为出厂默认
（2026-10-14 按各平台公开 API 模型列表核对：Kimi 平台当前无 k2.8 系模型 ID，旗舰通用模型为
`kimi-k3`；平台后续上新时用户可直接改填）。

### 5.6 TranscribeEngine（P1）

whisper.cpp（SwiftPM 集成，Metal 加速）；模型 `ggml-base`/`ggml-small` 在设置页显式下载
（SHA256 校验、断点续传）；产物 `transcript.json`（segments: start/end/text）；进度回调供 UI。

## 6. AX 与可测试性（落地 DESIGN_SYSTEM §4）

- 每个视图提交前自检：可交互控件都有 `accessibilityIdentifier`？动态行用稳定业务 id？
  图标按钮有中文 label？进度/开关暴露 AXValue？（对应 R1-R4、R6）
- 波形等自定义绘制包 `accessibilityElement`（R5）；快捷键表按 R7；Toast 走 `common.toast`（R8）；
- **UITests target**：P0 主流程（启动→发现 Sim→连接→列表→下载→校验→库内播放）脚本化，
  `xcodebuild test` 可跑，作为回归基线；
- **BuzzLinkSim 支持故障注入**（丢块/限速/中途断连），异常路径同样可脚本化验证；
- Accessibility Inspector Audit 无 error 级问题纳入 P0 验收。

## 7. 任务拆解（P0）

| # | 任务 | 预估 | 依赖 |
|---|---|---|---|
| T0 | 音频链 spike | ✅ 已完成 | — |
| T0.5 | BUZZLINK.md 补全（帧字段/命令集/常量） | ✅ 已完成 | — |
| T1 | BuzzLinkCore + XCTest | ✅ 已完成 | T0.5 |
| T2 | BuzzLink Central（BuzzLinkClient + BLE/loopback Transport；51 用例，含进程内 SimEngine 集成） | ✅ 已完成（2026-10-15） | T1 |
| T3 | BuzzLinkSim（SimEngine 纯逻辑 + 故障注入 + 内置合成样本；macOS/iOS 双平台 BLE 外设 + loopback TCP） | ✅ 已完成 | T1 |
| T4 | App 骨架（侧栏/设置窗口/AX id 全套/深色） | 1d | — |
| T5 | 设备页 + SyncEngine | 2d | T2、T4 |
| T6 | 录音库 + 播放器 | 2d | T4 |
| T7 | 协议控制台 + 向量导出 | 1d | T2、T4 |
| T8 | UITests 主流程 + Audit | 1d | T3、T5、T6 |

P1 随后：T9 转写、T10 AI 纪要、T11 导出扩展、T12 i18n。

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| BLE 吞吐有限（经验值 10-30 KB/s；T3 跨机实测 Mac↔iPhone Sim：MTU 515 下 568KB/24s ≈ **24 KB/s**；1h 录音约 20MB → 同步 10-30min） | 进度诚实展示 + 自动同步后台化；固件侧后续调 MTU/连接间隔/2M PHY |
| whisper 模型体积（base ≈142MB） | 设置页显式下载 + 进度 + 校验；不内置 |
| ad-hoc 分发的用户信任门槛（Gatekeeper 未签名提示） | README 构建指引；后续若有付费开发者账号再公证 |
| Sandbox 下 CBPeripheralManager（Sim） | ✅ T3 已验证：macOS 沙盒外设广播可用；新发现：**同机 Central 无法发现本机 Peripheral**（实测），对策 = loopback TCP 同机通道 + Sim 双平台跨机（TESTING.md §4） |
| 超长录音内存占用 | 波形/导出分块流式，禁止整段 PCM 驻留 |

## 9. 开放问题

1. ~~BUZZLINK.md 规范补全排期~~（已解决：T0.5 完成，T1/T2/T3 均按其落地）；
2. 库目录自定义位置（P1+，默认容器内即可）；
3. 英文界面排期（建议随 P1 一起，首版中文）。
