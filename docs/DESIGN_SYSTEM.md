# Beecord UX 设计系统（Design System）v1.0

> 状态：**v1.0 定稿**（2026-10-14）。适用范围：**Beecord Studio（macOS）** 与 **手表固件 UI（LVGL）**。
> 本文是 UI 实现的唯一依据；实现与本文冲突时，先改文档再写代码。
> 关系：[`BRAND.md`](BRAND.md)（品牌：为什么长这样）→ **本文**（令牌+组件：长什么样）
> → [`ux-design.md`](ux-design.md) / [`ux-device.md`](ux-device.md)（流程：怎么用）。
> 图版：§7 三张 PNG（源文件 SVG 同目录）。

## 1. 设计令牌（Design Tokens）

### 1.1 色彩

Mac 端优先用系统语义色（自动适配深浅色）；下表"品牌令牌"只在规定的品牌点使用。

| 令牌 | Mac Light | Mac Dark | 手表（AMOLED） | 用途 |
|---|---|---|---|---|
| `brand.primary` | #F5A623（浅底上的文字用 #D9820B） | #F5A623 | #F5A623 | 品牌露出点；**手表主操作色** |
| `accent.system` | systemBlue | systemBlue | —（用 brand.primary） | **Mac 常规主操作**（HIG） |
| `state.recording` | #E5484D | #E5484D | #E5484D | 录音态 / REC 指示 |
| `state.sync` | systemBlue | systemBlue | #0A84FF | 连接 / 同步 |
| `state.success` | systemGreen | systemGreen | #30D158 | 校验通过 / 完成 |
| `state.warning` | systemOrange | systemOrange | #FF9F0A | 低电 / 告警 |
| `state.danger` | systemRed | systemRed | #E5484D | 删除 / 错误 |
| `surface.bg` | windowBackground | （系统） | **#000000（纯黑省电）** | 页面背景 |
| `surface.card` | #FFFFFF | #1C1C1E | #1C1C1E | 卡片 |
| `surface.card2` | #F5F5F7 | #2C2C2E | #2C2C2E | 次级填充 / 轨道 |
| `text.primary` | labelColor | labelColor | #ECECF1 | 正文 |
| `text.secondary` | secondaryLabel | secondaryLabel | #9A9AA2 | 辅助 |
| `text.tertiary` | tertiaryLabel | tertiaryLabel | #6E6E73 | 弱提示 |
| `scrim.overlay` | rgba(0,0,0,.35) | rgba(0,0,0,.45) | rgba(0,0,0,.66) | 浮层遮罩 |

对比度底线：正文文字与背景 ≥ 4.5:1（琥珀底上的文字一律用墨黑 #101014，不用白）。

### 1.2 字体

**Mac（SwiftUI 映射）**——一律用系统样式，不写死字体名：

| 级别 | SwiftUI | 参考值 | 用途 |
|---|---|---|---|
| 页面标题 | `.title2`（bold） | 22/700 | 各页 H1 |
| 分组标题 | `.headline` | 13/600 | 卡片标题、表单分组 |
| 正文 | `.body` | 13/400 | 列表、说明 |
| 辅助 | `.callout` | 12/400 | 次级信息、表单注释 |
| 弱提示 | `.caption` | 11/400 | 表头、时间戳 |
| 等宽 | `.monospaced`（Menlo 12） | 12/400 | 控制台 hex、帧日志 |
| 数字 | `.monospacedDigit()` | 随级别 | 计时、进度、数值（禁止跳动变宽） |

**手表（LVGL）**：

| 级别 | 字体 / px / 字重 | 用途 |
|---|---|---|
| `display` | Montserrat 64 / 200（Light） | 表盘时间 |
| `timer` | Montserrat 56 / 200 | 录音计时 |
| `title` | 思源黑体 CN 15 / 600 | 页面标题 |
| `body` | 思源黑体 CN 13 / 400（英文数字走 Montserrat 13） | 列表、正文 |
| `caption` | 思源黑体 CN 11 / 400 | 辅助说明 |
| `tiny` | 思源黑体 CN 10 / 400 | **仅提示语**，禁用于正文与按钮 |

- 数字默认 tabular（Montserrat 数字天然等宽）；中文用 lv_font_conv 生成子集
  （常用 GB2312 字 + 项目词表，仅 11/13/15 三档，控制体积）。
- 行高：单倍行距 ×1.3；手表列表行内文字单行截断（…），不换行。

### 1.3 间距与圆角

- 间距刻度（4 的倍数）：`space.1=4 .2=8 .3=12 .4=16 .5=20 .6=24 .8=32`；组件内边距/间距只允许取这几个值。
- Mac：窗口默认 1060×720（最小 960×600）；主区 padding 22×26；侧栏宽 216（可调 180~240）。
- 手表：安全区内缩 上 14 / 左右 16 / 下 26（底部留给页面指示点）；页面 padding 16/16/30。
- 圆角：`radius.sm=6`（Mac 控件）、`.md=10`（Mac 卡片 / 手表行）、`.lg=14`（手表卡片）、`.xl=20`（浮层卡片）、`.full`（圆形/胶囊）。

### 1.4 动效

| 令牌 | 时长 | 用途 |
|---|---|---|
| `motion.micro` | 100ms | 按压反馈、开关 |
| `motion.fast` | 150ms | toast 进出、hover |
| `motion.normal` | 250ms | 页面切换、浮层 |
| `motion.slow` | 400ms | 大区域过渡（尽量少用） |

- 缓动：默认 ease-out；手表页面横滑 200ms ease-out；REC 点 pulse 1200ms 循环（透明度 1→0.25）。
- **禁止**：手表端弹性过冲（overshoot spring）；任何 >400ms 的动画；Mac 端与系统不一致的自定义窗口动画。

### 1.5 图标

- Mac：**SF Symbols**（weight regular；列表 16、按钮 14、空态 40）。约定用名：
  设备 `applewatch`、录音库 `waveform`、控制台 `terminal`、设置 `gear`、下载 `arrow.down.circle`、
  播放 `play.fill`、暂停 `pause.fill`、删除 `trash`、刷新 `arrow.clockwise`、校验 `checkmark.seal.fill`、
  同步 `antenna.radiowaves.left.and.right`、导出 `square.and.arrow.up`。
- 手表：**2px 圆头线性图标**（LV_SYMBOL_* 优先，缺位用同风格位图）；尺寸 16/20/24 三档。
- 两端都不混用"填充+线性"两套风格；不用 emoji 当功能图标。

### 1.6 反馈

- 手表：**提示音 + 屏幕视觉**（无震动马达），提示音事件表见 `ux-device.md` §8；可全局关。
- Mac：视觉反馈为主（toast/状态徽标）；完成类操作可放系统提示音（`NSSound`），不新增自定义音效。

## 2. Mac 组件规范（Beecord Studio / SwiftUI）

| 组件 | 规格 | AX id 示例（见 §4） |
|---|---|---|
| 侧栏导航项 | h28，r6；active：accent 15% 底 + accent 字 | `sidebar.navItem.devices` / `.library` / `.console` |
| 侧栏脚标行 | 状态点 8 + 12px 状态文字（截断）+ 右侧设置齿轮（13px，次色）；card2 底 r6；齿轮=打开标准 Settings 窗口 | `sidebar.deviceStatus`、`sidebar.settingsButton` |
| 主按钮 | h28，r6，accent 填充白字；disabled 透明度 45% | `device.downloadButton` |
| 次按钮 | h28，r6，1px 边框白底 | `device.refreshButton` |
| 危险按钮 | 红字；确认对话框内才允许红填充 | `library.deleteButton` |
| 小按钮 | h24，12px 字 | `library.exportButton` |
| 开关 / 复选 | 系统控件，不改样式 | `settings.autoSyncToggle`、`device.selectAllCheckbox` |
| 卡片 | r10，1px 边框，padding 16，卡间距 16 | — |
| 文件表行 | h28；选中 accent 6% 底；状态右对齐 | `device.fileTable.row.{fileName}` |
| 状态徽标 | 12px 文字：未下载 tertiary / 下载中 accent / ✓已校验 success / 失败 danger | `device.fileTable.row.{fileName}.status` |
| 进度条 | h6，r3，轨道 card2，填充 accent | `device.downloadProgress`（AXValue 0-1） |
| 标签页 | `.segmented` 分段控件 | `library.detailTabs` |
| 播放器 | 波形 h80；走带按钮 h36；倍速菜单 1/1.5/2；±15s | `library.player.playButton`、`.progressSlider` |
| Toast | 底部居中胶囊，12px，2s（错误 3s） | `common.toast` |
| 空态 | 图标 40 + 标题 15/600 + 说明 13 + 主按钮 | `{page}.emptyState` |
| 设置行 | 左标签右控件，行距 12 | `settings.aiProviderPicker` 等 |
| 控制台行 | Menlo 12；方向符号 →（上行）←（下行）；行点击展开 hex | `console.logTable` |
| 确认对话框 | NSAlert；主按钮写破坏性动词（"删除"）；**默认焦点=取消** | `common.confirmDialog` |
| 权限面罩 | 全局面罩：图标 40 + 标题 15/600 + 说明 13 + 主按钮（跳系统设置）+ 次按钮（重试） | `common.permissionOverlay`（蓝牙实例 `device.bluetoothOverlay`） |

### 2.1 页面骨架与布局规则（Mac / SwiftUI）

**页面骨架**（主区所有页面统一，自上而下）：

1. 页头：标题（`pageTitle`）+ 副标题（`body` / `textSecondary`），**左对齐**，padding 取 `Layout.mainPadding*`；
2. 内容区：**顶对齐**卡片流（`cardStyle()`，卡间距 16）；
3. 底部条：可选（如设备页整体进度条）。

**对齐规则**：

- 主内容一律**顶对齐**；**垂直+水平双居中仅限"满屏状态"**：扫描中、权限面罩。
- 空态卡片：顶对齐贴页头（卡片不随窗口纵向拉伸，下方留白），min-height 400
  （`Layout.emptyCardMinHeight`），卡内"图标+标题+说明+主按钮"聚簇居中。
- Web 原型默认文档流自顶向下，SwiftUI 容器默认居中——从原型落地时对齐方式必须**显式声明**，
  依赖默认值必然跑偏。

**SwiftUI 落地陷阱**（2026-10-16 设备页三个布局 bug 实录，均发生在 AX 全绿的代码上）：

1. 复用组件内部**禁止** `.frame(maxWidth/maxHeight: .infinity)` 贪婪扩张——组件一旦与兄弟
   视图组合，内部贪婪框架会吃掉全部剩余空间（空态按钮被挤到底部的根因）。**扩张归调用点**。
2. `VStack(alignment: .leading)` 只对齐子视图，不决定容器自身在父级中的位置；容器要靠
   `.frame(maxWidth: .infinity, alignment: .leading)` 或 Spacer 定位（页头被居中的根因）。
3. 单卡片放入带 `maxHeight: .infinity` 的容器时默认垂直居中，需 `Spacer(minLength: 0)`
   顶对齐（卡片悬空、距页头过远的根因）。

## 3. 手表组件规范（固件 / LVGL 9.5）

屏幕模型：410×502，圆角 r≈44；层级（z）：页面 0 < 指示点 5 < 熄屏层 60 < 确认 70 < 告警 75 < 关机层 80 < toast 90。

| 组件 | 规格 | 备注 |
|---|---|---|
| 状态栏 | h28，12px；BLE 点 8px（空心/实心）；电池壳 20×10 | 录音红/低电橙按令牌 |
| 大时间 | display 64/200，tabular | 表盘主页 |
| 状态小件 | 3 等分卡片，r14；标签 10 + 值 16/600 | 存储/待同步/最近录音 |
| 录音中横幅 | h36，r12，红边 1px + 深红底，12px | 点击进入录音页 |
| 页面指示点 | 6px 圆点 gap 6；当前=text.primary | 仅 3 个主页面 |
| 主按钮 | h56~64，r14 或圆形；琥珀底墨黑字 | 录音页/播放页主操作 |
| 按住确认 | 进度填充从左至右 **800ms** 完成才触发；文案"按住 XX" | 停止录音等高危操作 |
| 列表行 | h56，r12；标题 13/500 + 副标题 11；右 meta 11 | 录音列表 |
| 卡片 | r14，`surface.card`，padding 14 | 同步页等 |
| 设置行 | h48，r12；左 13 / 右 12 tertiary + › | 点击循环取值 |
| 进度条 | h4~6，r2~3；填充 brand 或 sync | 下载/同步 |
| 电平条 | 24 根，w6，r3，高 6%~100%，录音红 | 录音页 |
| Toast | 顶部居中胶囊，12px，1.6s；有提示音时前缀 ♪ | — |
| 确认浮层 | 中央卡片 w240，r16，padding 18；双按钮（取消 / 警告） | 删除等 |
| 状态图标 | 电池 <15% 橙、充电加闪电；BLE 空心/实心；无 SD 卡红条 | — |

触控目标：可点区域 **≥48×48px**；主操作 ≥64px。无 hover 态；按压态=缩放 0.95 或加深 10%。

## 4. 无障碍（AX）与可测试性（Mac）

**双目标**：① VoiceOver 基本可用；② **P0 流程 100% 可由 AX 树驱动**（不依赖坐标/截图识别），
供 AI 助手与自动化做手工/回归测试。

### 4.1 规则（强制）

- **R1 标识符**：所有可交互控件设 `accessibilityIdentifier`，命名 `{page}.{name}[.{qualifier}]`（lowerCamel）。
  页面前缀：`device.` / `library.` / `console.` / `settings.` / `sidebar.` / `common.`。
- **R2 稳定 id**：动态列表行用**稳定业务 id**（如文件名），禁止用行索引。
- **R3 标签**：图标按钮必须有中文动词短语 `accessibilityLabel`（"连接设备"、"下载所选"）。
- **R4 值**：进度/开关/滑块暴露 `AXValue`（0-1 / bool）；状态徽标为可读静态文本（测试可断言）。
- **R5 自定义绘制**（波形、电平）：包一层 `accessibilityElement`，暴露 label + value（如播放进度 0-1）。
- **R6 真控件**：行内操作必须是真实 button，不靠双击/手势等隐式交互。
- **R7 键盘可达**：Full Keyboard Access 下所有功能可操作；快捷键：⌘1/2/3 切侧栏页、⌘D 下载所选、
  ⌘R 刷新、空格 播放/暂停、⌘←/⌘→ ∓15s、⌘, 设置。
- **R8 Toast**：identifier `common.toast`，文本可被轮询断言。

### 4.2 工具与验收

- 工具：Xcode **Accessibility Inspector**（Audit）、**XCUITest**（UI 测试 target）、
  AppleScript / System Events（需在系统设置授予"辅助功能"权限）。
- 验收：P0 流程（扫描→连接→列表→下载→校验→播放→转写设置）各有一段 AX 驱动脚本
  （XCUITest 或 AppleScript）可跑通；Audit 无 error 级问题。
- 手表端无 AX：测试入口为串口日志 + BuzzLink 协议控制台（见 ux-design §3.5）。

## 5. 状态模式

| 模式 | Mac | 手表 |
|---|---|---|
| 空态 | 图标 40 + 标题 + 说明 + 主按钮 | 居中两行 13px 文案 |
| 加载 | spinner 16；列表用骨架行 | spinner / 进度条 |
| 错误 | 行内 danger 文本 + 【重试】；连续失败→卡片级提示 | 告警浮层（单按钮） |
| 确认（破坏性） | NSAlert，默认焦点=取消 | 确认浮层（取消/警告双钮）或按住确认 |
| Toast | 底部胶囊 2s（错误 3s） | 顶部胶囊 1.6s（可带 ♪） |
| 断连/离线 | 设备卡片灰显 + 自动重连提示 | BLE 图标空心 + toast |

## 6. 防跑偏规则（实现前必读）

1. 颜色只取 §1.1 令牌；需要新语义色时**先加令牌再用**。
2. 字号只取 §1.2 标尺；手表 `tiny` 仅提示语。
3. 间距/圆角只取 §1.3 刻度；禁止随手写 7、13、18 之类的值。
4. 分工不越界：Mac 主操作=系统蓝、琥珀仅品牌点；手表主操作=琥珀；红=录音/危险（两端一致）。
5. 手表背景必须纯黑；卡片外禁加阴影/发光/渐变（品牌资产除外）；Mac 阴影用系统材质。
6. 图标两端各一套风格（§1.5），不混用、不用 emoji。
7. 动效只取 §1.4 四档；手表禁 overshoot。
8. 破坏性操作必须二次确认，确认文案=动词+后果（"删除该录音？未同步副本无法恢复"）。
9. Mac 新组件必须带 §4 的 AX id；没有 id 的交互控件视为未完成。
10. 新增/修改组件：先更新本文与 §7 图版，再写实现代码。
11. 页面骨架与对齐遵守 §2.1：内容区顶对齐，仅满屏态（扫描中/权限面罩）可双居中；
    从原型落地时显式声明对齐，不靠默认值。
12. 布局/样式改动必须做 TESTING §4.5 截图自检并汇报对比结论；
    编译通过、AX 全绿均不代表布局正确。

## 7. 图版（实现对照用）

| 图版 | 内容 |
|---|---|
| [`design-system/ds-tokens.png`](design-system/ds-tokens.png) | 色彩 / 字体 / 间距圆角 / 动效令牌 |
| [`design-system/ds-mac-components.png`](design-system/ds-mac-components.png) | Mac 组件库（含状态与关键测量） |
| [`design-system/ds-watch-components.png`](design-system/ds-watch-components.png) | 手表组件库（410×502 1:1 解剖 + 组件） |
