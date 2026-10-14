# Beecord 品牌视觉规范（VI）v0.1

> 状态：**已定稿**（2026-10-14 评审通过，结论见 §6）。总览图：[`brand/png/brand-sheet.png`](brand/png/brand-sheet.png)。
> 所有图形的**源文件是 `brand/svg/` 下的 SVG**（手工维护），PNG 为渲染产物：
> `rsvg-convert -w 512 svg/beecord-mark.svg -o png/beecord-mark-512.png`（按目标尺寸改 `-w`）。

## 1. 标志

### 概念

主标志 = **蜂巢六边形 + 声波柱**：六边形取"蜂"（蜂巢 → Beecord 的 bee），声波柱取"录音"（record）；
六边形的工程感同时呼应 BuzzLink 协议帧魔数 `BE EC`（0xBE 0xEC）。**定稿方案 A**；
候选 B（抽象蜜蜂）/ C（字母 b 声纹）已弃用，仅存档于总览图 ①。

### 版本与文件

| 版本 | SVG 母版 | PNG | 用途 |
|---|---|---|---|
| 主标志（图标） | `svg/beecord-mark.svg` | `png/beecord-mark-512.png`、`-64.png` | 通用图标、头像、favicon |
| 锁定版 · 深色底 | `svg/beecord-logo-dark.svg` | `png/beecord-logo-dark.png` | 深色背景横排标志（README/官网） |
| 锁定版 · 浅色底 | `svg/beecord-logo-light.svg` | `png/beecord-logo-light.png` | 浅色背景横排标志 |
| 单色 · 墨黑 | `svg/beecord-mono-black.svg` | `png/beecord-mono-black.png` | 浅底单色（印刷/激光雕刻） |
| 单色 · 白 | `svg/beecord-mono-white.svg` | `png/beecord-mono-white.png` | 深底单色 |

### 安全区、最小尺寸与禁用

- 安全区：标志四周留白 ≥ 六边形宽度的 1/4。
- 最小尺寸：图标 ≥ 16 px；锁定版高度 ≥ 24 px（更小场景用单色版）。
- 禁止：拉伸变形、旋转、更改配色、添加投影/描边、置于与琥珀色对比不足的底图上。

## 2. 色彩

| 角色 | 色值 | 用途 |
|---|---|---|
| 琥珀（主色） | `#F5A623` | 品牌图形、主操作按钮；渐变 `#FFCE5C→#F0980A` 仅限标志与大图标 |
| 琥珀深 | `#D9820B` | 主色按压态、深色底上的强调文字 |
| 墨黑 | `#101014` | 浅色底正文、品牌底色 |
| 纸白 | `#F7F7F8` | 深色底正文、浅色底 |
| 录音红 | `#E5484D` | **仅功能色**：录音状态、危险/删除操作 |
| 同步蓝 | `#0A84FF` | 功能色：BLE 连接、同步 |
| 成功绿 | `#30D158` | 功能色：完成、CRC 校验通过 |
| 云雾灰 | `#8E8E93` | 次要文字、分隔线 |

设备端补充：AMOLED 界面背景用**纯黑 `#000000`**（像素熄屏省电）；墨黑仅用于非 AMOLED 场景。

平台分工：手表端以**琥珀为主操作色**（播放/主按钮），录音红仅用于录音态与危险操作；
Mac 端遵循 macOS HIG，常规操作沿用系统强调色（蓝），琥珀用于**品牌露出点**（侧栏标志、设备图标等）。

## 3. 字体

| 平台 | 字体 | 许可 |
|---|---|---|
| Mac（Beecord Studio） | SF Pro + PingFang SC | 系统自带 |
| 手表固件 | Montserrat（英文/数字）+ 思源黑体 CN（中文） | 均 SIL OFL，可裁剪子集随固件分发 |
| 计时/计数数字 | 等宽数字（tabular figures） | — |

## 4. 应用资产

| 资产 | 文件 | 说明 |
|---|---|---|
| Mac App 图标 | `png/app-icon-1024.png`（母版 `svg/app-icon.svg`） | Xcode 阶段转 `AppIcon.appiconset` |
| 手表开机画面 | `png/watch-splash.png`（母版 `svg/watch-splash.svg`） | 410×502 全尺寸，固件 UI 里程碑内置为 splash |
| BuzzLink 徽章 | `png/buzzlink-badge.png`（母版 `svg/buzzlink-badge.svg`） | 协议文档、协议控制台关于页 |

## 5. 命名与文字规范

- **Beecord**：项目/固件名，首字母大写；代码标识符、包名、命令全小写 `beecord`。
- **Beecord Studio**：macOS 客户端名。
- **BuzzLink**：BLE 同步协议名；帧魔数 `BE EC`（字节 `0xBE 0xEC`）。
- 文档用简体中文；界面文案先中文后英文（固件首版可仅中文）。

## 6. 评审结论（2026-10-14 已决）

1. 主标志定稿 **A（蜂巢声纹）**；候选 B / C 弃用，仅存档于总览图。
2. 原型配色已对齐：手表端主操作色改琥珀（录音/危险态保留红）；Mac 端沿用 HIG 系统蓝，
   琥珀落在品牌点（侧栏标志、设备图标）。
3. BuzzLink 徽章双环配色（琥珀 + 同步蓝）保留。
