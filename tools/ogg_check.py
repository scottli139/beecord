#!/usr/bin/env python3
"""Ogg 文件页结构 + CRC32 独立校验器（test_host 对拍用）。

纯 stdlib，独立实现直连型 CRC32（多项式 0x04C11DB7，初值 0，无输入/输出反射，
异或出 0；覆盖整页、crc 字段清零计算）——与固件 C 实现不共享任何代码，
用于 ogg_writer 产物的跨实现交叉验证。

用法：
    python3 ogg_check.py <file.ogg>

全部页结构合法且 CRC 正确：打印页数摘要，exit 0；
任何一页出错：打印页号与原因，exit 1。
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

PAGE_HEADER_LEN = 27
CAPTURE_PATTERN = b"OggS"


class OggCheckError(Exception):
    """页结构非法或 CRC 校验失败：信息含页号定位。"""


def _make_crc_table() -> list[int]:
    """直连型 CRC32 查表：table[i] = (i << 24) 按位推进 8 次。"""
    table = []
    for i in range(256):
        crc = i << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
        table.append(crc)
    return table


_CRC_TABLE = _make_crc_table()


def crc32_direct(data: bytes, crc: int = 0) -> int:
    """直连型 CRC32：初值 0，无反射，异或出 0。"""
    for byte in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[((crc >> 24) ^ byte) & 0xFF]
    return crc


def parse_pages(data: bytes) -> list[tuple[int, int, int, int, int, int]]:
    """逐页解析，返回 (page_seq, header_type, granule, serial, num_segments, payload_len)。

    每页同步验算 CRC32；结构非法或 CRC 不符抛 OggCheckError（含页号）。
    """
    pages = []
    offset = 0
    while offset < len(data):
        page_no = len(pages)
        header = data[offset : offset + PAGE_HEADER_LEN]
        if len(header) < PAGE_HEADER_LEN:
            raise OggCheckError(f"第 {page_no} 页：文件截断（剩 {len(header)} 字节，不足页头 27）")
        if header[0:4] != CAPTURE_PATTERN:
            raise OggCheckError(f"第 {page_no} 页：capture pattern 非法（{header[0:4].hex()}，应为 4f 67 67 53）")
        version = header[4]
        if version != 0:
            raise OggCheckError(f"第 {page_no} 页：version {version} 非法（应为 0）")
        header_type = header[5]
        granule = struct.unpack("<q", header[6:14])[0]
        serial, seq, stored_crc = struct.unpack("<III", header[14:26])
        num_segments = header[26]
        seg_table = data[offset + PAGE_HEADER_LEN : offset + PAGE_HEADER_LEN + num_segments]
        if len(seg_table) < num_segments:
            raise OggCheckError(f"第 {page_no} 页：文件截断（段表不完整）")
        payload_len = sum(seg_table)
        total = PAGE_HEADER_LEN + num_segments + payload_len
        page = data[offset : offset + total]
        if len(page) < total:
            raise OggCheckError(f"第 {page_no} 页：文件截断（剩 {len(page)} 字节，应 {total}）")

        crc_body = page[:22] + b"\x00\x00\x00\x00" + page[26:]
        if crc32_direct(crc_body) != stored_crc:
            raise OggCheckError(
                f"第 {page_no} 页：CRC32 不符（存 {stored_crc:08x}，算 {crc32_direct(crc_body):08x}）"
            )
        pages.append((seq, header_type, granule, serial, num_segments, payload_len))
        offset += total
    if not pages:
        raise OggCheckError("文件不含任何页")
    return pages


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path = Path(argv[1])
    try:
        data = path.read_bytes()
    except OSError as exc:
        print(f"ogg_check: 无法读取 {path}: {exc}", file=sys.stderr)
        return 1
    try:
        pages = parse_pages(data)
    except OggCheckError as exc:
        print(f"ogg_check: {path}: {exc}", file=sys.stderr)
        return 1

    seqs = [p[0] for p in pages]
    serials = {p[3] for p in pages}
    seq_continuous = seqs == list(range(seqs[0], seqs[0] + len(seqs)))
    print(
        f"ogg_check: {len(pages)} 页全部校验通过（serial={serials.pop():#010x}，"
        f"seq {seqs[0]}..{seqs[-1]}{'' if seq_continuous else '（不连续！）'}，"
        f"CRC32=直连型 0x04C11DB7）"
    )
    if not seq_continuous:
        print("ogg_check: 页序号不连续", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
