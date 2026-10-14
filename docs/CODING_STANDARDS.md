# Beecord 编码标准（Coding Standards）

> 状态：**v1.0 定稿**（2026-10-14）。适用于本仓库全部代码与文档：**所有贡献者（人类与 AI）同等遵守**。
> 测试相关要求另见 [`TESTING.md`](TESTING.md)；视觉与 AX 规则见 [`DESIGN_SYSTEM.md`](DESIGN_SYSTEM.md)。

## 1. 通用原则

1. **正确性 > 简洁 > 技巧**。不为了少几行引入理解成本；与所在文件的既有风格保持一致。
2. **标识符英文，注释与文档中文**。公共 API 的文档注释写"契约"（参数/返回/错误/线程模型），
   不写复述代码的注释；历史、原因、出处交给 git 提交信息。
3. **无魔法数**：协议常量集中 `BuzzLinkConstants`（Swift）/ `buzz_proto.h`（C）；
   设计常量集中 `DesignTokens`（Swift，值取自 DESIGN_SYSTEM §1）。
4. **零三方依赖（P0）**：任何新依赖（含 SwiftPM 包、IDF 组件）先开 issue 讨论再引入。
   例外白名单：whisper.cpp（P1）、ESP-IDF 官方组件仓库组件。
5. **合规红线**：本仓库是独立开源项目——不得提交任何第三方商业产品的商标、
   专有协议常量、二进制产物或逆向工程产物；不得提交密钥、证书、个人数据、录音样本
   （合成测试样本除外）。
6. **构建产物与本地文件不入库**：`build/`、`DerivedData/`、`.DS_Store`、模型文件、
   大于 5MB 的二进制（确有必要先讨论）。

## 2. Swift（apps/mac，Swift 6，macOS 15+）

- **并发**：开启严格并发检查；UI 状态用 `@Observable`（不混用 `ObservableObject`）；
  异步一律 `async/await` + 结构化并发，BLE/IO 回调统一桥接为 `AsyncStream`，禁止散养 `Task`。
- **类型**：`struct` 优先；`class` 仅用于引用语义或框架要求。访问级别最小化——
  仅 SDK 公开 API 标 `public`，其余默认 `internal`。
- **命名**：类型 `UpperCamelCase`，成员 `lowerCamelCase`，布尔 `is/has/should` 前缀；
  协议名按能力命名（`FrameParsing`），不用 `I` 前缀。
- **错误处理**：可失败路径用 `throws` + 具名 `Error` 枚举（带 `LocalizedError` 描述）；
  **禁止** `try!`、`!` 强解包、数组越界硬编码（测试代码与 XCTest 断言除外）。
- **日志**：`os.Logger` 分级（`.debug/.info/.error`），禁止向仓库提交 `print`。
- **SwiftUI 视图**：单一职责，单文件超 150 行考虑拆分；样式只取 `DesignTokens`，
  不内联颜色/字号/间距字面量。
- **AX 强制**：所有可交互控件必须 `.accessibilityIdentifier`，命名遵守
  DESIGN_SYSTEM §4 R1-R8；**没有 AX id 的交互控件视为未完成**。
- **格式**：4 空格缩进（Xcode 默认）；文件头不写作者/日期注释块；`// MARK: -` 分区适度。

## 3. C（firmware，C11，ESP-IDF v5.4）

- **组件结构**：遵循 IDF 组件布局（`CMakeLists.txt` + `include/` + 源文件）；公开头文件
  用 `buzz_` 前缀 + include guard + `extern "C"` 包裹。
- **命名**：函数/变量 `snake_case`，宏与枚举值 `UPPER_SNAKE_CASE`，类型 `_t` 结尾。
- **协议层硬件无关**：`main/proto/` 下代码**禁止 include 任何 `esp_*` 头文件**，
  不调用 IDF API——必须能在 macOS 宿主用 gcc/clang 编译跑单测（见 TESTING.md L1）。
- **内存**：启动期完成分配，运行路径零 `malloc`；禁递归（栈深受限）；缓冲区长度显式传递。
- **错误码**：组件内统一返回 `esp_err_t` 风格（`BUZZ_OK = 0`，负值为错）。
- **警告**：`-Wall -Wextra` 零警告；格式化以仓库根 `.clang-format` 为准（提交前执行）。

## 4. Python（tools/，3.11+）

PEP 8；公共函数带 type hints；stdlib 优先，三方依赖写入 `requirements.txt` 并说明用途。

## 5. 文档与插图

- 设计文档中文；Markdown 表格优先于长段落（协议字段、命令集等）。
- **禁止 ASCII 示意图**：需要架构/时序/流程图时，写图源（Graphviz `.dot` / HTML），
  渲染 PNG 后用 ReadMediaFile 读回检查（无乱码/截断/重叠）才定稿；图源与 PNG 同目录保存，
  文档用相对路径引用。

## 6. Git 提交

- 提交信息**英文**、祈使句（`Add frame reassembly`）；一个提交只做一件事；
  正文（可选）说明 *why*，不复述 *what*。
- 提交前自检：编译零警告、相关测试通过（见 TESTING.md 门禁）、改动涉及的设计文档已同步。

## 7. 完成定义（Definition of Done）

一项改动算"完成"当且仅当：

1. 编译通过、零警告（Swift 严格并发检查无新告警）；
2. 新增/变更逻辑有对应测试且全部通过（TESTING.md §6 门禁）；
3. 涉及 UI 的改动：AX id 齐全、DESIGN_SYSTEM 无冲突；
4. 涉及协议/设计的改动：对应规范文档已同步更新；
5. 不违反 §1.5/§1.6 红线。
