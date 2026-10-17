# Beecord 测试要求（Testing）

> 状态：**v1.0 定稿**（2026-10-14）。与 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) 配套。
> 方针：**自动化测试为主，AI 手工测试为一线手段**；协议行为用测试向量锁死，UI 行为用 AX 树锁死。

## 1. 测试分层

| 层 | 范围 | 工具 | 依赖硬件 |
|---|---|---|---|
| **L1 单元** | BuzzLinkCore（帧/CRC/重组/向量）、固件协议层 | XCTest；gcc/clang 宿主单测 | 否 |
| **L2 组件** | BLE 会话状态机、下载重组、SyncEngine 逻辑 | BuzzLinkSim（回环 + 故障注入） | 否 |
| **L3 端到端** | P0 用户流程、异常路径 | UITests（AX 驱动）+ AI 手工测试 | 否（Sim） |
| **L4 实机** | 固件 ↔ Studio 真实 BLE 链路、吞吐与稳定性 | 开发板 + 手动清单 | 是 |

L1-L3 全部不依赖硬件，任何贡献者本机可跑；L4 由维护者在开发板上执行（清单见 §7）。

## 2. 标准命令

```bash
# Mac 端 L1：BuzzLinkCore 单测（SwiftPM 包测试，独立于 Xcode 工程）
cd apps/mac/Packages/BuzzLink && swift test

# Mac 端 L2+L3：Studio/Sim 构建 + UI 测试（Xcode 工程）
cd apps/mac && xcodebuild test -scheme BeecordStudio -destination 'platform=macOS'

# 固件协议层宿主单测（L1）
cd firmware/test_host && make test
```

本地 SwiftPM 包的测试 target 不属于 xcodeproj scheme，故 L1 与 L2/L3 分两条命令；
三个命令都必须在提交前本地通过；CI 门禁工作流 `mac-test`（`.github/workflows/mac-test.yml`）
执行前两条（固件宿主单测待 M0 后接入）。

> 调试附记：`swift test` 输出重定向到文件/管道时 XCTest stdout 为块缓冲，
> 不可流式观察（T2 调试期实测）；需要实时输出时直接前台跑。

## 3. 测试向量（协议行为的唯一事实来源）

- 向量格式为 **JSON**（可读性优先），schema 定义在 BuzzLinkCore（字段：命名的时间戳、
  方向 `tx/rx`、帧 type、channel、payload hex、注释 `comment`）；一文件一会话。
- **合成向量入库**：`Packages/BuzzLink/Tests/BuzzLinkCoreTests/Fixtures/`（Mac 侧）与经转换脚本生成的 C 表
  （固件侧，工具在 `tools/`）。同一份 JSON 两端消费，保证 Mac 与固件对协议的理解一致。
- BuzzLink 协议控制台导出的会话可直接作为向量回放（格式同源，见 ux-design §3.5）。
- 修改协议行为 = 先更新 [`BUZZLINK.md`](BUZZLINK.md) 与向量，再改实现。

## 4. AI 手工测试规范（一线手段）

UI 的自动化验收以 **AX 树驱动** 为准，自动化回归中禁止坐标点击与截图比对（脆弱且不可评审）。
布局/样式改动的一次性人工验收除外——必须做截图自检，见 §4.5。

1. **入口**：DESIGN_SYSTEM §4 R1-R8。可交互控件必有稳定 `accessibilityIdentifier`；
   进度/开关暴露 `AXValue`；状态徽标为可读文本（可断言）；Toast 走 `common.toast`。
2. **驱动方式**：XCUITest（首选，脚本入库 `apps/mac/BeecordStudioUITests/`）；
   探索性场景可用 AppleScript / JXA（需授予辅助功能权限，脚本放 `apps/mac/Tests/ax/`）。
3. **对端**：一律使用 BuzzLinkSim（虚拟设备），不依赖实体硬件；Sim 的故障注入开关
   覆盖异常路径（见 §5）。
   - **传输通道**：同机联调/UITests 走 **loopback TCP**（`127.0.0.1:40404`，Sim 默认开启）——
     平台限制：同一台 macOS 上 Central 无法发现本机 Peripheral 的 BLE 广播（半双工无线电
     不收自身发射时隙，CoreBluetooth 无软件回环，2026-10-15 实测确认），因此 Sim 提供
     与空口同构的本地字节流通道（帧协议不变，§3.3 重组天然适配流式 socket）。
     设备流与库流 UITest 均由测试进程内 UITestSimServer 供设备源（端口冲突自动 XCTSkip）；
     库流设备源为**进程内合成 Ogg 静音流**（SyntheticOgg，零文件依赖——UITest runner 沙箱
     读仓库 Fixtures 会被 TCC 拒绝，120s 仅 18KB，转码链全路径可消费）。
     **CI 现场取证**（2026-10-16，教训：CI 失败 5/5 无窗口）：app 侧 `LaunchTrace`
     仅在 `-libraryRoot` 注入时写 `<libraryRoot>/uitest-launch-trace.log`，用例失败时
     回读进失败信息；工作流失败时自动上传 xcresult（截图/AX 树/屏幕录像）为 artifact。
     启动后若主窗口未随启动出现，用例兜底 `activate + ⌘N` 强制新建主窗口。
     **CI 环境三条认知**（2026-10-17 固化，排障先核对）：
     1. CI runner 屏幕恒 **1024×768**、可视区约 **1024×674**（菜单栏 + Dock 占用）——
        任何"窗口大于可视区导致控件被推出屏外/侧栏被折叠"的表象先查这一条
        （`-uiTestWindow` 小窗档即为此设：min 900×560 / ideal 1080×680）；
     2. **WindowGroup 在 CI 不自动建主窗**（恢复态无窗口可还原时静默跳过建窗，
        本机手动 `open -a` 亦可复现）——每个用例启动后须探测侧栏/窗口，缺失则
        `activate + ⌘N` 兜底；
     3. 该恢复模式**骨架类与库页类 UITest 都需要**（`BeecordStudioUITests` 与
        `DeviceSyncUITests` 的 launch helper 均已内置；新增用例类照抄同款）。
     **慢 runner 时序加固**（2026-10-17）：`SyncEngineRecoveryTests.
     testDisconnectMidDownloadReconnectsAndResumes` 曾因 30s 轮询窗口被节流拉伸击穿
     flake 一次（rerun 自过）——重连+整文件重下轮询上限放宽至 60s（≈名义 8.5x 裕量，
     仍有界轮询非固定 sleep，断言语义不变）；同类先例见 `testCancelSyncStopsQueue`。
   - **真 BLE 链路**：Sim 跑在另一台 Mac 或 iPhone/iPad（同一 App 双平台 target）上，
     作为 Studio 跨机联调与固件到货前的 BLE 演练对端。
   - **跨机实测（2026-10-16 已闭环，T5）**：`BLE_LIVE_SMOKE=1 swift test --filter
     BLELiveSmokeTests` 跑 BLE 全会话（扫描/连接/INFO/LIST/全量下载/CRC 验收 + 吞吐日志），
     Mac Central ↔ iPhone Sim 实测 ≈39.8 KB/s；该实测发现并修复了 BLETransport 的
     跨 actor 断言崩溃（@preconcurrency 同步见证在非主队列调用 SIGTRAP）。
     无环境变量时自动 skip，不进 CI；`LoopbackLiveSmokeTests` 只覆盖 loopback 通道。
4. **流程**：启动 Sim → 启动 Studio → 按 P0 流程脚本执行 → 断言 AXValue/静态文本/库目录产物。
5. **缺口处理**：发现无法通过 AX 表达的操作 = 实现缺陷，先补 AX id 再测；
   发现的问题记录为 GitHub issue（附脚本与断言输出）。

**P0 流程必须 100% AX 可驱动**（DESIGN_SYSTEM §4.2 验收项）：扫描 → 连接 → 列表 →
勾选/全选下载 → 进度 → CRC 校验通过 → 库内出现 → 播放 → 暂停 → 导出 → 设置读写。

## 4.5 布局视觉自检（截图自检，布局/样式改动必做）

自动化回归禁止截图比对；但任何布局/样式改动（frame/padding/对齐/卡片结构）的一次性验收
**必须**实拍截图人工对比——AX 断言验证元素**存在**，验证不了元素**位置**
（2026-10-16 设备页空态按钮沉底 / 页头被居中 / 卡片悬空三例，全部发生在 UITest 9/9 全绿的
代码上）。方法（AI 执行，约 1 分钟）：

1. **环境清场**（跑 UITest 前同样必做）：`pkill -f MacOS/BeecordStudio`（进程名**无空格**）、
   `killall BuzzLinkSim`、`lsof -nP -iTCP:40404 -sTCP:LISTEN` 应为空。
   残留 app 实例会抢 AX 归属，导致用例随机失败（实录 2 例）。
   手工验收注记（2026-10-17 实录）：CLI 构建产物若被签名（含 app-sandbox entitlement，
   如 UITest 探测后），`-libraryRoot` 指向的任意目录写入会被 kernel deny（`/tmp`、
   `$TMPDIR` 均不豁免）——手动验证需重签去掉 sandbox（`codesign --force --sign -
   --entitlements <无 sandbox 的 plist>`）或以未签名产物直接运行可执行文件。
2. `open -a <DerivedData 下的 BeecordStudio.app>` → 等目标状态稳定
   （空态需等自动连接失败回落，约 15s）→ AppleScript 取窗口框
   （AX 里进程名**有空格**）：
   `osascript -e 'tell application "System Events" to tell process "Beecord Studio" to get {position, size} of window 1'`
   → `screencapture -Rx,y,w,h -o /tmp/shot.png` → ReadMediaFile 检查。
3. 原型对照：headless Chrome 截同场景（场景值见原型页面右上角场景选择器）：
   `"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --disable-gpu --screenshot=/tmp/proto.png --window-size=1280,800 "file://$PWD/docs/prototype/index.html?state=<场景>"`
4. 对比结论连同门禁结果一并汇报；用户要求时展示截图。
5. **动态态（hover）验证**：静态截图覆盖不了指针悬停态。方法——同一命令链内
   `open -a` 置前目标 app（screencapture 抓的是坐标处**顶层窗口**，命令间隙终端可能被
   重新置前导致误拍，2026-10-16 实录一例）→ 合成事件触发 hover
   （`CGWarpMouseCursorPosition` 不产生追踪区域事件，必须用
   `CGEventCreateMouseEvent(kCGEventMouseMoved)` + `CGEventPost`）→ 截图 + 像素采样比对
   （`fill.hover` 仅 5% 透明度，肉眼不可靠，用采样数值断言）。

## 5. 故障注入矩阵（Sim 必须支持）

| 注入 | 期望行为 |
|---|---|
| 随机丢块（如 2%） | CRC 校验失败 → 自动整文件重下（≤2 次）→ 行内标注重试过程 |
| 限速（1 KB/s） | 进度/速率/剩余时间诚实更新，UI 不冻结 |
| 推流中途断连 | 文件标"未完成"，重连后可重新下载，整体进度暂停并提示 |
| 末帧 CRC 错误 | 行内红色标记 + 重试入口；连续失败出卡片级提示 |
| 空文件列表 | 设备页空态文案 |
| 超长文件名 / 非法字符 | UI 截断不溢出；入库文件名安全化 |
| 存储将满（>90%） | 同步前警告条 |

每条注入场景至少有一个 L2/L3 自动化用例或 AI 手工测试脚本。

## 6. 提交门禁（DoD 的测试部分）

1. `xcodebuild test` 与 `make test` 全绿；
2. 新增/变更逻辑带测试：**协议层无测试不合并**；
3. P0 流程脚本（受影响的）重跑通过；Accessibility Inspector Audit 无 error 级问题；
4. 覆盖率目标：BuzzLinkCore 行覆盖 ≥ 90%（帧编解码、CRC、重组要求 100%）；
   状态机迁移全覆盖；UI 不测像素、不测布局坐标。

## 7. L4 实机验收清单（维护者执行）

板卡到位后、每个里程碑出口前执行：烧录 → 广播可见（服务 UUID）→ Studio 连接 →
同步 ≥ 3 个不同长度录音（含 >10min）→ CRC 全过 → 断连恢复 → 连续同步 30min 无异常 →
记录吞吐（KB/s）与失败率到里程碑记录。

## 8. 测试数据纪律

- 测试音频一律**合成样本**（脚本生成 16kHz 单声道 Ogg Opus：`tools/make_sim_samples.sh`，
  产物提交在 `apps/mac/BuzzLinkSim/Resources/Samples/`）；
  真实录音、个人数据不入库。
- 向量中的文件名/设备名使用虚构值（如 `REC_20260101_120000`、`BCD-DEMO`）。
