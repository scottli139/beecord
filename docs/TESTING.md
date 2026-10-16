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

UI 的验收以 **AX 树驱动** 为准，禁止坐标点击与截图比对（脆弱且不可评审）。

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
     设备流 UITest 由测试进程内 UITestSimServer 供设备源（端口冲突自动 XCTSkip）。
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
