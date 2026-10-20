#!/usr/bin/env bash
# Beecord 手表 UI 字体子集生成脚本（M5.1，词表法）。
#
# 产物（入仓，构建不依赖 node/网络）：
#   main/ui/fonts/ui_font_cjk10.c    思源黑体 CN Regular 10px（tiny：提示语/状态小件标签）
#   main/ui/fonts/ui_font_cjk11.c    思源黑体 CN Regular 11px（caption：状态栏/辅助说明）
#   main/ui/fonts/ui_font_cjk13.c    思源黑体 CN Regular 13px（body：日期/正文）
#   main/ui/fonts/ui_font_cjk15b.c   思源黑体 CN Bold    15px（title：页面标题/小件值，代设计 16/600）
#   main/ui/fonts/ui_font_num56.c    Montserrat Light  56px（timer：录音计时，数字+冒号）
#   main/ui/fonts/ui_font_num64.c    Montserrat Light  64px（display：表盘时间，数字+冒号）
#
# 用法：bash gen.sh   （依赖 node>=18 与 npx，首次自动下载字体源到 vendor/）
set -euo pipefail

GEN_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR="$GEN_DIR/vendor"
OUT_DIR="$GEN_DIR/../../main/ui/fonts"
CHARSET_FILE="$GEN_DIR/charset.txt"

# npm 字体包（上游均为 OFL 许可；本机 GitHub raw 直连不可用，npm registry 稳定可达。
# 上游 canonical 源见 README.md 的 license 注记）
NPM_SHANS_PKG="@fontpkg/source-han-sans@2.5.2"   # Source Han Sans（= 思源黑体，Adobe/Google 同源）
NPM_MONTS_PKG="@expo-google-fonts/montserrat@0.4.2" # Montserrat（Google Fonts 同款静态实例）
SHANS_REG="SourceHanSans-Regular.otf"
SHANS_BOLD="SourceHanSans-Bold.otf"
MONTS_LIGHT="Montserrat_300Light.ttf"

mkdir -p "$VENDOR" "$OUT_DIR"

# ---------- 1. 下载字体源（幂等，已存在则跳过） ----------
fetch_pkg() { # $1=npm 包标识  $2=要提取的文件（相对包内路径，可多个）
    local pkg="$1"; shift
    if [ -f "$VENDOR/$1" ]; then return 0; fi
    echo ">> 下载 $pkg"
    local tmp; tmp="$(mktemp -d)"
    (cd "$tmp" && npm pack "$pkg" --silent >/dev/null)
    local tgz; tgz="$(ls "$tmp"/*.tgz | head -1)"
    for f in "$@"; do
        tar xzf "$tgz" -C "$tmp" "package/$f"
        cp "$tmp/package/$f" "$VENDOR/$(basename "$f")"
    done
    rm -rf "$tmp"
}

[ -f "$VENDOR/$SHANS_REG" ] && [ -f "$VENDOR/$SHANS_BOLD" ] || \
    fetch_pkg "$NPM_SHANS_PKG" "$SHANS_REG" "$SHANS_BOLD"
[ -f "$VENDOR/$MONTS_LIGHT" ] || \
    fetch_pkg "$NPM_MONTS_PKG" "300Light/$MONTS_LIGHT"

# ---------- 2. 词表 = charset.txt 去重 + ASCII 可打印区 ----------
# 去掉注释与空白行，逐字去重（python3 保证 Unicode 按码点处理）
CHARS="$(python3 - "$CHARSET_FILE" <<'PY'
import sys
text = open(sys.argv[1], encoding="utf-8").read()
chars = set()
for line in text.splitlines():
    line = line.split("#", 1)[0]
    chars.update(line.strip())
chars.update(chr(c) for c in range(0x20, 0x7F))  # ASCII printable
print("".join(sorted(chars)))
PY
)"
echo ">> 词表字符数: ${#CHARS}"

# ---------- 3. 子集化生成 ----------
conv() { # $1=源字体 $2=字号 $3=产物名 $4=词表
    echo ">> $3"
    npx --yes lv_font_conv@latest \
        --font "$1" --size "$2" --bpp 4 --format lvgl --no-compress \
        --symbols "$4" \
        --output "$OUT_DIR/$3.c"
}

conv "$VENDOR/$SHANS_REG"   10 ui_font_cjk10  "$CHARS"
conv "$VENDOR/$SHANS_REG"   11 ui_font_cjk11  "$CHARS"
conv "$VENDOR/$SHANS_REG"   13 ui_font_cjk13  "$CHARS"
conv "$VENDOR/$SHANS_BOLD"  15 ui_font_cjk15b "$CHARS"

# 大数字仅需 0-9 与冒号（设计：timer/display 为纯数字计时）
NUM_CHARS="0123456789:"
conv "$VENDOR/$MONTS_LIGHT" 56 ui_font_num56 "$NUM_CHARS"
conv "$VENDOR/$MONTS_LIGHT" 64 ui_font_num64 "$NUM_CHARS"

echo ">> 完成，产物："
ls -l "$OUT_DIR"/ui_font_*.c
echo ">> 总体积: $(du -ck "$OUT_DIR"/ui_font_*.c | tail -1 | cut -f1) KB（目标 <200KB）"
