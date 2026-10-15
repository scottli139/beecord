#!/bin/bash
# 生成 BuzzLinkSim 内置合成录音样本（TESTING.md §8：测试音频一律合成样本）。
# 产物：16 kHz 单声道 Ogg Opus ~44kbps，提交入库供 Sim app 作为虚拟文件库使用。
# 用法：tools/make_sim_samples.sh [输出目录]（默认 apps/mac/BuzzLinkSim/Resources/Samples）
# 依赖：ffmpeg（brew install ffmpeg）

set -euo pipefail

OUT_DIR="${1:-$(dirname "$0")/../apps/mac/BuzzLinkSim/Resources/Samples}"
FFMPEG="${FFMPEG:-ffmpeg}"

if ! command -v "$FFMPEG" >/dev/null 2>&1; then
    echo "error: ffmpeg 未找到（可用 FFMPEG 环境变量指定路径）" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

encode() {
    local name="$1" filter="$2"
    "$FFMPEG" -hide_banner -loglevel error -y \
        -f lavfi -i "$filter" \
        -ac 1 -c:a libopus -b:a 44k -vbr on \
        "$OUT_DIR/$name"
    echo "generated: $OUT_DIR/$name"
}

# 5s：440 Hz 正弦（短样本，冒烟用）
encode sample-05s.ogg "sine=frequency=440:sample_rate=16000:duration=5"

# 30s：440+660 Hz 双音（中等样本，常规下载路径）
encode sample-30s.ogg \
    "sine=frequency=440:sample_rate=16000:duration=30[a];sine=frequency=660:sample_rate=16000:duration=30,volume=0.5[b];[a][b]amix=inputs=2:normalize=0"

# 120s：粉红噪声 + 4 Hz 颤音（长样本，分片/进度/稳定性路径）
encode sample-120s.ogg \
    "anoisesrc=duration=120:sample_rate=16000:color=pink:amplitude=0.3,tremolo=f=4:d=0.7"
