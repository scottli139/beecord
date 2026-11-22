#!/usr/bin/env python3
"""Beecord 手表 UI 字体子集覆盖检查（M5.3 防再发机制）。

扫描 main/ui/*.c 字符串字面量中的全部非 ASCII 字符（CJK + 全角标点 +
符号箭头），与 font_gen/charset.txt 词表比对；另检查 num56/num64 生成物
覆盖数字、冒号与未对时占位符连字符。缺字即报错退出（CI/test_host
`make fontcheck` 可跑）。新增 UI 文案后忘补字符集会在构建期被拦下
（回写 DESIGN_SYSTEM.md 规则 14）。
用法: python3 check_font_charset.py
"""
import re
import sys
from pathlib import Path

UI_DIR = Path(__file__).resolve().parent.parent / "main" / "ui"
FONT_DIR = UI_DIR / "fonts"
CHARSET = Path(__file__).resolve().parent / "font_gen" / "charset.txt"
NUMERIC_FONT_NAMES = ("ui_font_num56.c", "ui_font_num64.c")
NUMERIC_REQUIRED = frozenset("0123456789:-")

# 所有非 ASCII 字符都需字形：CJK 表意文字 + 全角标点（，。！？、；：""（）…）
# + 箭头/符号（→› 等）。只按 CJK 区间匹配会漏全角标点（M5.3 实机豆腐块实锤：
# "无存储卡，无法录音"的"，"U+FF0C 不在 CJK 区间）。日志里的非 ASCII 会被
# 一并收入词表——多收无害（子集极小），漏收才出豆腐块。
NON_ASCII = re.compile(r"[^\x00-\x7f]")

# 只扫字符串字面量（注释里的中文不上屏，不要求字形）
STRING_LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')
GLYPH_COMMENT = re.compile(r"/\* U\+([0-9A-Fa-f]+) ")


def ui_chars():
    chars = set()
    hits = {}
    # main/ui 下全部 .c：页面（scr_*）、toast（ui_toast）以及 toast 文案的
    # 调用方（ui_rec/ui_task 里的 ui_toast_show 字符串同样上屏——M5.3 漏扫
    # ui_rec.c 导致"无存储卡，无法录音"的"，"缺字形）。printf/ESP_LOG 串口
    # 输出不上屏，但多收进词表无害，不再做文件级排除。
    # 全量扫 main/ui/*.c（M5.3 教训：ui_rec/ui_task 内联调 ui_toast_show 的
    # 中文字符串会漏扫——宁可日志/printf 过度包含，不漏上屏文案）
    for f in sorted(UI_DIR.glob("*.c")):
        text = f.read_text(encoding="utf-8")
        found = set()
        for lit in STRING_LIT.findall(text):
            found |= set(NON_ASCII.findall(lit))
        for c in found:
            hits.setdefault(c, f"{f.name}")
        chars |= found
    return chars, hits


def charset_chars():
    text = CHARSET.read_text(encoding="utf-8")
    chars = set()
    for line in text.splitlines():
        line = line.split("#", 1)[0]
        chars.update(line.strip())
    return chars


def numeric_font_errors():
    errors = []
    for name in NUMERIC_FONT_NAMES:
        text = (FONT_DIR / name).read_text(encoding="utf-8")
        chars = {chr(int(codepoint, 16)) for codepoint in GLYPH_COMMENT.findall(text)}
        missing = sorted(NUMERIC_REQUIRED - chars)
        if missing:
            errors.append(f"{name}: {' '.join(missing)}")
    return errors


def main():
    ui, hits = ui_chars()
    cs = charset_chars()
    missing = sorted(ui - cs)
    if missing:
        print(f"缺字 {len(missing)} 个（UI 用了但 charset.txt 未收录）：")
        for c in missing:
            print(f"  {c}  ({hits[c]})")
        print("→ 把这些字补进 firmware/tools/font_gen/charset.txt 后重跑 gen.sh")
        return 1
    numeric_errors = numeric_font_errors()
    if numeric_errors:
        print("数字字体缺字（num56/num64 必须包含 0-9、冒号和连字符）：")
        for error in numeric_errors:
            print(f"  {error}")
        print("→ 检查 firmware/tools/font_gen/gen.sh 的 NUM_CHARS 后重跑 gen.sh")
        return 1
    print(f"字体子集覆盖检查通过（UI 中文字符 {len(ui)} 个、数字字体占位符全收录）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
