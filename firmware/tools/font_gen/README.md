# font_gen —— 手表 UI 字体子集生成（词表法）

LVGL 字体生成流水线。手表 Flash 资源紧张，中文按 **UI 文案词表 + ASCII 可打印区**
子集化（DESIGN_SYSTEM §1.2；M5_PLAN D7 决策，"常用 GB2312 字"收敛为逐子步收集的词表），
生成物为 `lv_font_conv` 输出的 LVGL v9 字体 C 表，**入仓到 `main/ui/fonts/`，
固件构建不依赖 node/网络**。

## 产物

| 文件 | 源字体 | 字号/字重 | 用途（theme.h 宏） |
|---|---|---|---|
| `ui_font_cjk10.c` | 思源黑体 CN Regular | 10/400 | `FONT_TINY`——提示语、状态小件标签 |
| `ui_font_cjk13.c` | 思源黑体 CN Regular | 13/400 | `FONT_BODY`——日期、横幅、正文 |
| `ui_font_cjk15b.c` | 思源黑体 CN Bold | 15/700 | `FONT_TITLE`——页面标题、状态小件值（代设计 16/600，无对应字重） |
| `ui_font_num56.c` | Montserrat Light | 56/300 | `FONT_TIMER`——录音计时（数字+冒号） |
| `ui_font_num64.c` | Montserrat Light | 64/300 | `FONT_DISPLAY`——表盘时间（数字+冒号） |

参数：bpp=4（抗锯齿灰度）；大数字仅含 `0-9:`；中文三档共用 `charset.txt` 词表。

## 用法

```bash
bash beecord/firmware/tools/font_gen/gen.sh
```

依赖：node ≥18（`npx` 自动取 `lv_font_conv`）。字体源自动下载并缓存到 `vendor/`
（不入库）。新增 UI 文案时：把新汉字补进 `charset.txt` → 重跑 `gen.sh`。
体积超标（目标 <200KB 二进制）时裁剪词表到必要字符。

## 字体来源与许可

- **思源黑体 CN（Source Han Sans）**：Adobe/Google 联合开发，本工具经 npm 包
  `@fontpkg/source-han-sans@2.5.2` 取得静态 OTF（canonical 源：
  <https://github.com/adobe-fonts/source-han-sans> releases）。
  许可：**SIL Open Font License 1.1**——可自由使用/嵌入/再分发（含二进制），
  保留版权与许可声明即可。
- **Montserrat**：Julieta Ulanovsky 等，本工具经 npm 包
  `@expo-google-fonts/montserrat@0.4.2` 取得静态 TTF（canonical 源：
  <https://github.com/google/fonts> `ofl/montserrat`，当前为可变字体，
  上游历史提交保留静态字重）。
  许可：**SIL Open Font License 1.1**。

两款字体均为 OFL，固件二进制嵌入与开源仓再分发无合规障碍。

## 缺字防护（M5.3 起）

`make -C test_host fontcheck`（已挂进 `make test`）扫描 `main/ui/scr_*.c` 与
`ui_toast.c` 的字符串字面量中文字符，对 `charset.txt` 词表做比对，缺字报错。
新增 UI 文案忘补词表会在构建期被拦下（DESIGN_SYSTEM 防跑偏规则 14）。
