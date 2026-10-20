#!/usr/bin/env python3
"""Beecord 固件 REPL 辅助：读提示符后发命令并收集输出（USB Serial/JTAG）。

用法（须在 IDF 环境内，且主机 python 有 pyserial——IDF python_env 自带）：
    python3 repl.py <端口> <命令...> [命令...]  [--timeout 秒]

注意 dsrdtr=False：host 开关 DTR 会复位芯片；先等到 "beecord>" 提示符再发命令。
"""
import argparse
import sys
import time

import serial


def wait_prompt(ser: serial.Serial, timeout: float) -> None:
    deadline = time.time() + timeout
    buf = b""
    last_kick = 0.0
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            # 窗口需盖住 boot 尾部两行日志（提示符后还可能跟启动日志）
            buf = (buf + chunk)[-512:]
            if b"beecord>" in buf:
                return
        else:
            time.sleep(0.01)
        # 首次空行可能撞上芯片 boot（USB 枚举前字节被丢弃），周期重发
        if time.time() - last_kick > 1.0:
            ser.write(b"\r\n")
            ser.flush()
            last_kick = time.time()
    raise TimeoutError("等不到 beecord> 提示符（设备未启动/非 REPL 端口？）")


def run_cmd(ser: serial.Serial, cmd: str, timeout: float) -> str:
    time.sleep(0.3)
    ser.reset_input_buffer()  # 清掉上一条的残留提示符/日志，避免误判结束
    ser.write(cmd.encode() + b"\r\n")
    ser.flush()
    deadline = time.time() + timeout
    out = bytearray()
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            out += chunk
            if out.rstrip().endswith(b"beecord>"):
                break
        else:
            time.sleep(0.02)
    text = out.decode("utf-8", "replace")
    # 剥离命令回显与尾部提示符
    lines = text.replace("\r\n", "\n").split("\n")
    body = [l for l in lines if l.strip() and not l.strip().startswith("beecord>")]
    if body and body[0].strip() == cmd:
        body = body[1:]
    return "\n".join(body).strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("commands", nargs="+")
    ap.add_argument("--timeout", type=float, default=20.0, help="单命令最长等待秒数")
    ap.add_argument("--boot-timeout", type=float, default=30.0, help="等提示符秒数")
    args = ap.parse_args()

    last_exc = None
    for attempt in range(5):
        try:
            ser = serial.Serial(args.port, 115200, dsrdtr=False, timeout=0.1)
            break
        except OSError as exc:
            last_exc = exc
            time.sleep(2)
    else:
        raise last_exc
    try:
        # macOS 打开 cu 设备可能保持 DTR 有效 → 芯片被复位信号按住；显式释放。
        ser.setDTR(False)
        ser.setRTS(False)
        # REPL 提示符只在启动时打印一次；发空行换一个新提示符再读
        ser.write(b"\r\n")
        ser.flush()
        wait_prompt(ser, args.boot_timeout)
        for cmd in args.commands:
            print(f"beecord> {cmd}")
            print(run_cmd(ser, cmd, args.timeout))
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
