#!/usr/bin/env python3
"""将 lv_font_conv 生成的数字字体转换为真正的 tabular 数字。

项目使用的 Montserrat 源字体是比例数字：仅统一 advance 可以固定后续字符位置，
但窄字形会贴左；因此同时调整 ofs_x，把原始位图居中到统一数字位宽内。
只处理 0–9，保留冒号、连字符、位图、字号和基线。

用法：
    python3 tabularize.py ui_font_num56.c ui_font_num64.c
    python3 tabularize.py --check ui_font_num56.c ui_font_num64.c
"""

import argparse
import re
from pathlib import Path

EXPECTED_CODEPOINTS = [0x2D, *range(0x30, 0x3A), 0x3A]
GLYPH_COMMENT = re.compile(r"/\* U\+([0-9A-Fa-f]+) ")
GLYPH_DSC = re.compile(
    r"    \{\.bitmap_index = (?P<bitmap>\d+), \.adv_w = (?P<adv>\d+),"
    r" \.box_w = (?P<box_w>\d+), \.box_h = (?P<box_h>\d+),"
    r" \.ofs_x = (?P<ofs_x>-?\d+), \.ofs_y = (?P<ofs_y>-?\d+)\}(?P<tail>[^\n]*)"
)


def _parse(text):
    codepoints = [int(value, 16) for value in GLYPH_COMMENT.findall(text)]
    if codepoints != EXPECTED_CODEPOINTS:
        raise ValueError("字符集或顺序不符合数字字体约定，应为 -、0–9、:")

    descriptors = list(GLYPH_DSC.finditer(text))
    if len(descriptors) != len(EXPECTED_CODEPOINTS) + 1:
        raise ValueError(
            f"glyph 描述数量异常：期望 {len(EXPECTED_CODEPOINTS) + 1}，"
            f"实际 {len(descriptors)}"
        )
    # 第 0 项保留，其后依次是连字符、0–9、冒号。
    return descriptors[2:12]


def _centered_ofs_x(advance, box_w):
    # advance 为 1/16 px；四舍五入到最近的整数像素 left bearing。
    return (advance - 16 * box_w + 16) // 32


def normalize_font(text):
    """返回等宽化后的 C 源码；重复执行结果不变。"""
    digits = _parse(text)
    common_advance = max(int(item.group("adv")) for item in digits)
    replacements = []

    for item in digits:
        box_w = int(item.group("box_w"))
        values = {
            "bitmap": int(item.group("bitmap")),
            "adv": common_advance,
            "box_w": box_w,
            "box_h": int(item.group("box_h")),
            "ofs_x": _centered_ofs_x(common_advance, box_w),
            "ofs_y": int(item.group("ofs_y")),
        }
        replacement = (
            "    {{.bitmap_index = {bitmap}, .adv_w = {adv}, .box_w = {box_w},"
            " .box_h = {box_h}, .ofs_x = {ofs_x}, .ofs_y = {ofs_y}}}{tail}"
        ).format(tail=item.group("tail"), **values)
        replacements.append((item.span(), replacement))

    for (start, end), replacement in reversed(replacements):
        text = text[:start] + replacement + text[end:]
    return text


def tabular_errors(text):
    """返回生成物中比例数字回潮或字形未居中的错误列表。"""
    digits = _parse(text)
    advances = {int(item.group("adv")) for item in digits}
    errors = []
    if len(advances) != 1:
        errors.append(f"0–9 advance 不一致：{sorted(advances)}")

    common_advance = max(advances)
    for digit, item in zip("0123456789", digits):
        advance = int(item.group("adv"))
        box_w = int(item.group("box_w"))
        ofs_x = int(item.group("ofs_x"))
        if abs(32 * ofs_x + 16 * box_w - advance) > 16:
            errors.append(f"数字 {digit} 未在固定位宽内居中")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="只检查，不回写文件")
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()

    failed = False
    for path in args.paths:
        try:
            text = path.read_text(encoding="utf-8")
            normalized = normalize_font(text)
            if args.check:
                if normalized != text:
                    raise ValueError("数字字体尚未等宽化，请重跑 gen.sh")
            elif normalized != text:
                path.write_text(normalized, encoding="utf-8")
            advances = {int(item.group("adv")) for item in _parse(normalized)}
            print(f"{path}: tabular 0–9，adv_w={max(advances)}")
        except (OSError, ValueError) as exc:
            print(f"{path}: {exc}")
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
