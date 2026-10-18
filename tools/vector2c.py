#!/usr/bin/env python3
"""BuzzLink 会话测试向量（buzzlink-vector/1）转 C 表（TESTING.md §3）。

同一份 JSON 两端消费：Mac 侧 XCTest 直读 JSON，固件侧由本脚本生成 C 表回放。
本脚本**独立实现**帧封装与 CRC-16/CCITT-FALSE（多项式 0x1021，初值 0xFFFF，
无反射）——与固件 C 实现不共享任何代码，生成的完整帧字节供 test_host 逐字节
对拍，实现跨语言交叉验证。

用法：
    python3 vector2c.py <input.json> <out_dir>

输出（out_dir 下，名字取 JSON 文件名，非字母数字统一转下划线）：
    vectors_<name>.c / vectors_<name>.h —— entry 数组（direction/type/session/
    channel/payload 字节/完整帧字节/comment）
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

SCHEMA = "buzzlink-vector/1"

# 帧格式常量（BUZZLINK §3，与固件 buzz_proto.h 对应但各自实现）
MAGIC = bytes((0xBE, 0xEC))
VERSION = 0x01
FRAME_HEADER_LEN = 10
FRAME_CRC_LEN = 2
FRAME_MAX_PAYLOAD = 232  # MTU 247 - 3 - 12

_DIR_TX = 0
_DIR_RX = 1


class VectorError(Exception):
    """向量文件非法：信息面向修改 JSON 的人。"""


def _make_crc_table() -> list[int]:
    """CRC-16/CCITT-FALSE 查表：table[i] = (i << 8) 按位推进 8 次。"""
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        table.append(crc)
    return table


_CRC_TABLE = _make_crc_table()


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    """增量 CRC-16/CCITT-FALSE；新会话从 0xFFFF 起。"""
    for byte in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[((crc >> 8) ^ byte) & 0xFF]
    return crc


def build_frame(frame_type: int, session: int, channel: int, payload: bytes) -> bytes:
    """独立实现帧封装：magic | version | type | session | flags | channel(LE)
    | length(LE) | payload | crc16(LE)，CRC 覆盖 version..payload。"""
    if len(payload) > FRAME_MAX_PAYLOAD:
        raise VectorError(
            f"payload 长 {len(payload)} 超单帧上限 {FRAME_MAX_PAYLOAD}，"
            "向量条目不支持分片（需拆消息或走 FRAG 专用向量）"
        )
    body = bytes(
        (
            VERSION,
            frame_type,
            session,
            0x00,  # flags：会话向量均为不分片消息
            channel & 0xFF,
            (channel >> 8) & 0xFF,
            len(payload) & 0xFF,
            (len(payload) >> 8) & 0xFF,
        )
    ) + payload
    crc = crc16(body)
    return MAGIC + body + bytes((crc & 0xFF, (crc >> 8) & 0xFF))


def _parse_hex_int(value: object, field: str, index: int) -> int:
    """type/channel/session 宽容解析：\"0x30\" / \"30\"（hex 字符串）或 JSON 数字。

    与 BuzzLinkTestVector.decodeHexInteger（Swift）语义一致。
    """
    if isinstance(value, str):
        text = value.strip()
        if text.lower().startswith("0x"):
            text = text[2:]
        try:
            return int(text, 16)
        except ValueError:
            raise VectorError(f"entries[{index}].{field}: 非法 hex 整数 {value!r}") from None
    if isinstance(value, int):
        return value
    raise VectorError(f"entries[{index}].{field}: 期望 hex 字符串或整数，实际 {value!r}")


def _parse_payload_hex(text: object, index: int) -> bytes:
    """payloadHex：容忍空白、大小写；偶数长度、仅 hex 字符。"""
    if not isinstance(text, str):
        raise VectorError(f"entries[{index}].payloadHex: 期望字符串")
    stripped = re.sub(r"\s+", "", text)
    if len(stripped) % 2 != 0 or not re.fullmatch(r"[0-9a-fA-F]*", stripped):
        raise VectorError(f"entries[{index}].payloadHex: 非法 hex {text!r}")
    return bytes.fromhex(stripped) if stripped else b""


def _check_range(value: int, maximum: int, field: str, index: int) -> int:
    if not 0 <= value <= maximum:
        raise VectorError(f"entries[{index}].{field}: {value} 超范围 0..{maximum}")
    return value


def load_vector(path: Path) -> list[dict[str, object]]:
    """读入并校验向量 JSON，返回标准化 entry 列表。"""
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise VectorError(f"无法解析 {path}: {exc}") from exc
    if not isinstance(doc, dict):
        raise VectorError("向量文件顶层必须是 JSON 对象")

    schema = doc.get("schema")
    if schema != SCHEMA:
        raise VectorError(f"不支持的向量 schema：{schema!r}（当前仅 {SCHEMA}）")
    entries = doc.get("entries")
    if not isinstance(entries, list):
        raise VectorError("缺少 entries 数组")

    normalized: list[dict[str, object]] = []
    for index, raw in enumerate(entries):
        if not isinstance(raw, dict):
            raise VectorError(f"entries[{index}]: 期望对象")
        direction = raw.get("direction")
        if direction not in ("tx", "rx"):
            raise VectorError(f"entries[{index}].direction: 期望 \"tx\" 或 \"rx\"，实际 {direction!r}")
        frame_type = _check_range(_parse_hex_int(raw.get("type"), "type", index), 0xFF, "type", index)
        channel = _check_range(
            _parse_hex_int(raw.get("channel"), "channel", index), 0xFFFF, "channel", index
        )
        # session 缺省 = 0：与 Swift 侧 makeFrame(defaultSession: 0) 语义一致
        session_raw = raw.get("session")
        session = (
            0
            if session_raw is None
            else _check_range(_parse_hex_int(session_raw, "session", index), 0xFF, "session", index)
        )
        payload = _parse_payload_hex(raw.get("payloadHex", ""), index)
        comment = raw.get("comment")
        normalized.append(
            {
                "direction": _DIR_TX if direction == "tx" else _DIR_RX,
                "type": frame_type,
                "session": session,
                "channel": channel,
                "payload": payload,
                "frame": build_frame(frame_type, session, channel, payload),
                "comment": comment if isinstance(comment, str) else "",
            }
        )
    return normalized


def _c_ident(name: str) -> str:
    """文件名转 C 标识符片段：非字母数字转下划线、下划线化驼峰基名。"""
    return re.sub(r"[^0-9A-Za-z]+", "_", name).strip("_") or "session"


def _c_camel(name: str) -> str:
    """basic_session → BasicSession（符号命名用）。"""
    return "".join(part.capitalize() for part in name.split("_") if part)


def _c_string(text: str) -> str:
    """字符串转 C 字面量内容（处理引号与反斜杠；UTF-8 中文原样保留）。"""
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def _emit_bytes(symbol: str, data: bytes, note: str) -> list[str]:
    """生成 static const 字节数组定义；空数组用 1 字节占位（C11 无零长数组）。"""
    lines = []
    if not data:
        lines.append(f"static const uint8_t {symbol}[1] = {{ 0x00 }}; // 空 {note} 占位")
        return lines
    lines.append(f"static const uint8_t {symbol}[{len(data)}] = {{")
    for offset in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in data[offset : offset + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    return lines


def render(entries: list[dict[str, object]], stem: str) -> tuple[str, str]:
    """生成 .h/.c 文本对。符号前缀取自向量文件名（basic_session 等）。"""
    snake = _c_ident(stem)
    camel = _c_camel(snake)
    guard = f"VECTORS_{snake.upper()}_H"
    table_sym = f"kVectors{camel}"
    count_sym = f"kVectors{camel}Count"

    header = f"""// 由 tools/vector2c.py 生成（源：{stem}.json）；请勿手改，改 JSON 后重新生成。
#ifndef {guard}
#define {guard}

#include <stddef.h>
#include <stdint.h>

// 方向（以 Central/客户端为视角）：tx = Central→Device，rx = Device→Central
#define BUZZ_VECTOR_DIR_TX 0u
#define BUZZ_VECTOR_DIR_RX 1u

// 一条会话向量：字段语义与 buzzlink-vector/1 schema 一一对应。
// frame 为完整帧字节（magic…payload…crc），由 Python 独立封装实现生成。
typedef struct {{
    uint8_t direction;
    uint8_t type;
    uint8_t session;
    uint16_t channel;
    uint32_t payload_len;
    const uint8_t *payload;
    uint32_t frame_len;
    const uint8_t *frame;
    const char *comment;
}} buzz_vector_entry_t;

extern const buzz_vector_entry_t {table_sym}[];
extern const size_t {count_sym};

#endif // {guard}
"""

    body: list[str] = [
        f'// 由 tools/vector2c.py 生成（源：{stem}.json）；请勿手改，改 JSON 后重新生成。',
        f'#include "vectors_{snake}.h"',
        "",
    ]
    table_rows = []
    for index, entry in enumerate(entries):
        payload_sym = f"kVec{camel}{index}Payload"
        frame_sym = f"kVec{camel}{index}Frame"
        body.extend(_emit_bytes(payload_sym, entry["payload"], "payload"))  # type: ignore[arg-type]
        body.extend(_emit_bytes(frame_sym, entry["frame"], "frame"))  # type: ignore[arg-type]
        body.append("")
        comment = _c_string(entry["comment"])  # type: ignore[arg-type]
        table_rows.append(
            f"    {{ {entry['direction']}u, 0x{entry['type']:02X}u, 0x{entry['session']:02X}u, "
            f"0x{entry['channel']:04X}u, {len(entry['payload'])}u, {payload_sym}, "  # type: ignore[arg-type]
            f"{len(entry['frame'])}u, {frame_sym}, \"{comment}\" }},"  # type: ignore[arg-type]
        )

    body.append(f"const buzz_vector_entry_t {table_sym}[] = {{")
    body.extend(table_rows)
    body.append("};")
    body.append("")
    body.append(f"const size_t {count_sym} = {len(entries)}u;")
    body.append("")
    return header, "\n".join(body)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    input_path = Path(argv[1])
    out_dir = Path(argv[2])
    try:
        entries = load_vector(input_path)
        stem = _c_ident(input_path.stem)
        header, source = render(entries, stem)
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / f"vectors_{stem}.h").write_text(header, encoding="utf-8")
        (out_dir / f"vectors_{stem}.c").write_text(source, encoding="utf-8")
    except VectorError as exc:
        print(f"vector2c: 错误: {exc}", file=sys.stderr)
        return 1
    print(f"vector2c: {len(entries)} 条向量 → {out_dir}/vectors_{stem}.c/.h")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
