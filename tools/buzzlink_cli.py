#!/usr/bin/env python3
"""BuzzLink BLE command-line client (firmware M4.3).

Minimal BuzzLink central for exercising the device control channel with a real
watch: scans for BCD-*, connects, subscribes to Event Notify, and speaks the
frame protocol end to end (BUZZLINK.md):

    1. encode/decode BE EC frames (CRC-16/CCITT-FALSE via binascii.crc_hqx,
       covering version..payload per section 3.1);
    2. reassemble FRAG shards (fragIndex/fragCount, section 3.3);
    3. run the info / list / fileinfo subcommands and print the JSON plus
       round-trip time.

Usage:
    python3 buzzlink_cli.py info
    python3 buzzlink_cli.py list [--offset N] [--limit N]
    python3 buzzlink_cli.py fileinfo <id>

Exit code 0 = success, 1 = any check failed. Requires bleak. On macOS the
terminal needs Bluetooth permission (System Settings > Privacy & Security >
Bluetooth).
"""

from __future__ import annotations

import argparse
import asyncio
import binascii
import json
import struct
import sys
import time

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

# GATT definition, BUZZLINK.md section 2.1 (canonical text form; bleak lowercases).
SERVICE_UUID = "0E242009-7EF4-4AE0-B3B0-E3A59E58D47C"
CMD_WRITE_UUID = "A2268D42-7024-430F-B059-9A156436973D"
EVENT_NOTIFY_UUID = "C22080F5-0E96-4823-8909-1DD2CE427D2D"

# Frame constants (section 3.1 / 5.1).
FRAME_MAGIC = b"\xbe\xec"
FRAME_VERSION = 0x01
FLAG_FRAG = 0x01
CHANNEL_CONTROL = 0x0001
TYPE_INFO = 0x03
TYPE_INFO_GET = 0x04
TYPE_LIST_REQ = 0x10
TYPE_LIST_RESP = 0x11
TYPE_FILE_INFO_REQ = 0x20
TYPE_FILE_INFO_RESP = 0x21

COMMAND_TIMEOUT_S = 5.0
INFO_PUSH_TIMEOUT_S = 3.0


class ProtocolError(Exception):
    """Raised when the device answer violates the BuzzLink protocol."""


def frame_crc(body: bytes) -> int:
    """CRC-16/CCITT-FALSE over version..payload (binascii.crc_hqx, init 0xFFFF)."""
    return binascii.crc_hqx(body, 0xFFFF)


def encode_frame(ftype: int, session: int, channel: int, payload: bytes) -> bytes:
    """Encode one BE EC frame: header + body + little-endian CRC trailer."""
    body = struct.pack("<BBBBHH", FRAME_VERSION, ftype, session, 0, channel,
                       len(payload))
    body += payload
    return FRAME_MAGIC + body + struct.pack("<H", frame_crc(body))


class FrameStream:
    """Byte-stream parser: resyncs on magic, validates CRC, reassembles FRAG.

    Feed arbitrary notify chunks; complete messages arrive via ``messages``
    as (type, session, channel, payload) tuples, oldest first.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._frags: dict[tuple[int, int, int], dict[int, bytes]] = {}
        self.messages: list[tuple[int, int, int, bytes]] = []

    def feed(self, data: bytes) -> None:
        self._buf += data
        while True:
            # magic resync: drop garbage before the next BE EC
            start = self._buf.find(FRAME_MAGIC)
            if start < 0:
                self._buf.clear()
                return
            if start > 0:
                del self._buf[:start]
            if len(self._buf) < 12:  # header 10 + crc 2 minimum
                return
            ftype = self._buf[3]
            session = self._buf[4]
            flags = self._buf[5]
            channel = struct.unpack_from("<H", self._buf, 6)[0]
            length = struct.unpack_from("<H", self._buf, 8)[0]
            total = 10 + length + 2
            if len(self._buf) < total:
                return
            body = bytes(self._buf[2 : 10 + length])
            (crc,) = struct.unpack_from("<H", self._buf, 10 + length)
            del self._buf[:total]
            if crc != frame_crc(body):
                print("WARN  frame crc mismatch, dropped (stream stays aligned)")
                continue
            payload = body[8:]
            if flags & FLAG_FRAG:
                index, count = struct.unpack_from("<HH", payload, 0)
                self._merge_frag(ftype, session, channel, index, count, payload[4:])
            else:
                self.messages.append((ftype, session, channel, payload))

    def _merge_frag(self, ftype: int, session: int, channel: int, index: int,
                    count: int, shard: bytes) -> None:
        key = (ftype, session, channel)
        bucket = self._frags.setdefault(key, {})
        bucket[index] = shard
        if len(bucket) != count:
            return
        try:
            payload = b"".join(bucket[i] for i in range(count))
        except KeyError:
            return  # out-of-range shard index; never complete
        del self._frags[key]
        self.messages.append((ftype, session, channel, payload))


def decode_json(payload: bytes, context: str) -> dict:
    try:
        obj = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError(f"{context}: invalid JSON: {exc}") from exc
    if not isinstance(obj, dict):
        raise ProtocolError(f"{context}: expected JSON object")
    return obj


class BuzzDevice:
    """Connected-device wrapper: session bookkeeping + response waiters."""

    def __init__(self, client: BleakClient) -> None:
        self._client = client
        self._stream = FrameStream()
        self._session = 0

    async def __aenter__(self) -> "BuzzDevice":
        await self._client.start_notify(EVENT_NOTIFY_UUID, self._on_notify)
        return self

    async def __aexit__(self, *exc: object) -> None:
        await self._client.stop_notify(EVENT_NOTIFY_UUID)

    def _on_notify(self, _char: object, data: bytearray) -> None:
        self._stream.feed(bytes(data))

    def _next_session(self) -> int:
        self._session = (self._session + 1) & 0xFF
        if self._session == 0:
            self._session = 1
        return self._session

    async def command(self, ftype: int, channel: int, payload: bytes,
                      resp_type: int) -> tuple[dict, float]:
        """Send one control-channel command and await its JSON response.

        Returns (json_dict, elapsed_seconds). Raises ProtocolError on timeout
        or a malformed/ERROR answer.
        """
        session = self._next_session()
        deadline = time.monotonic() + COMMAND_TIMEOUT_S
        await self._client.write_gatt_char(
            CMD_WRITE_UUID, encode_frame(ftype, session, channel, payload), response=False)
        started = time.monotonic()
        while time.monotonic() < deadline:
            for msg in list(self._stream.messages):
                mtype, msession, mchannel, mpayload = msg
                if mtype == 0x7F and msession == session and mchannel == channel:
                    self._stream.messages.remove(msg)
                    err = decode_json(mpayload, "ERROR")
                    raise ProtocolError(f"ERROR frame code={err.get('code')} "
                                        f"message={err.get('message')!r}")
                if (mtype, msession, mchannel) != (resp_type, session, channel):
                    continue
                self._stream.messages.remove(msg)
                return decode_json(mpayload, f"type 0x{resp_type:02X}"), \
                    time.monotonic() - started
            await asyncio.sleep(0.02)
        raise ProtocolError(f"timeout {COMMAND_TIMEOUT_S}s waiting for "
                            f"type 0x{resp_type:02X} session={session}")

    async def wait_proactive_info(self) -> dict:
        """Await the session=0 INFO push that follows subscription (section 6.1)."""
        deadline = time.monotonic() + INFO_PUSH_TIMEOUT_S
        while time.monotonic() < deadline:
            for msg in list(self._stream.messages):
                mtype, msession, _channel, mpayload = msg
                if mtype != TYPE_INFO or msession != 0:
                    continue
                self._stream.messages.remove(msg)
                return decode_json(mpayload, "proactive INFO")
            await asyncio.sleep(0.02)
        raise ProtocolError(f"no proactive INFO within {INFO_PUSH_TIMEOUT_S}s")


async def scan_for_device(prefix: str, timeout: float) -> BLEDevice:
    print(f"scanning for {prefix}* ({timeout:.0f}s) ...")
    adv_map = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for device, adv in adv_map.values():
        name = adv.local_name or device.name or ""
        if name.startswith(prefix):
            print(f"  found {name} ({device.address})")
            return device
    raise ProtocolError(f"no device named {prefix}* found")


def print_json(obj: dict, elapsed: float | None = None) -> None:
    suffix = f"  [{elapsed * 1000:.0f} ms]" if elapsed is not None else ""
    print(json.dumps(obj, ensure_ascii=False, indent=2) + suffix)


def check(cond: bool, message: str) -> None:
    if not cond:
        raise ProtocolError(f"check failed: {message}")


async def cmd_info(dev: BuzzDevice, _args: argparse.Namespace) -> None:
    pushed = await dev.wait_proactive_info()
    print("proactive INFO (session=0, after subscribe):")
    print_json(pushed)
    got, elapsed = await dev.command(TYPE_INFO_GET, CHANNEL_CONTROL, b"", TYPE_INFO)
    print("INFO_GET response:")
    print_json(got, elapsed)
    for key in ("model", "fwVersion", "protocol", "deviceName", "battery", "charging",
                "storageTotal", "storageFree"):
        check(key in pushed, f"proactive INFO missing field {key!r}")
        check(key in got, f"INFO_GET response missing field {key!r}")
    for key in ("model", "fwVersion", "protocol", "deviceName", "battery", "charging"):
        check(pushed[key] == got[key],
              f"field {key!r} differs: push={pushed[key]!r} get={got[key]!r}")


async def cmd_list(dev: BuzzDevice, args: argparse.Namespace) -> None:
    req: dict[str, int] = {"offset": args.offset}
    if args.limit is not None:
        req["limit"] = args.limit
    page, elapsed = await dev.command(TYPE_LIST_REQ, CHANNEL_CONTROL,
                                      json.dumps(req).encode(), TYPE_LIST_RESP)
    print(f"LIST_RESP offset={page.get('offset')} "
          f"({len(page.get('files', []))}/{page.get('total')} entries):")
    print_json(page, elapsed)
    check(page.get("status") == "ok", f"unexpected status: {page.get('status')!r}")
    check(isinstance(page.get("files"), list), "missing files array")
    for entry in page["files"]:
        check("id" in entry and "durationMs" in entry and "size" in entry,
              f"entry missing fields: {entry!r}")


async def cmd_fileinfo(dev: BuzzDevice, args: argparse.Namespace) -> None:
    req = json.dumps({"id": args.id}).encode()
    info, elapsed = await dev.command(TYPE_FILE_INFO_REQ, CHANNEL_CONTROL, req,
                                      TYPE_FILE_INFO_RESP)
    print(f"FILE_INFO_RESP id={args.id!r}:")
    print_json(info, elapsed)
    check(info.get("status") == "ok", f"unexpected status: {info.get('status')!r} "
                                      f"code={info.get('code')} message={info.get('message')!r}")
    for key in ("size", "crc16", "durationMs", "format", "sampleRate", "channels"):
        check(key in info, f"missing field {key!r}")


COMMANDS = {"info": cmd_info, "list": cmd_list, "fileinfo": cmd_fileinfo}


async def run(args: argparse.Namespace) -> None:
    device = await scan_for_device(args.name_prefix, args.timeout)
    client = BleakClient(device, timeout=args.timeout)
    try:
        await client.connect()
    except Exception as exc:  # bleak raises platform-specific subclasses
        raise ProtocolError(f"connect: {exc!r}") from exc
    print(f"connected (mtu={client.mtu_size})")
    async with BuzzDevice(client) as dev:
        # let the subscription settle so the device sees CCCD write before we talk
        await asyncio.sleep(0.3)
        await COMMANDS[args.command](dev, args)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="scan/connect timeout in seconds (default: 10)")
    parser.add_argument("--name-prefix", default="BCD-",
                        help="expected device name prefix (default: BCD-)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("info", help="await proactive INFO and compare with INFO_GET")
    p_list = sub.add_parser("list", help="list one page of recordings")
    p_list.add_argument("--offset", type=int, default=0)
    p_list.add_argument("--limit", type=int, default=None)
    p_info = sub.add_parser("fileinfo", help="query metadata of one recording")
    p_info.add_argument("id", help="file id, e.g. REC_0001.OGG")
    args = parser.parse_args(argv[1:])
    try:
        asyncio.run(run(args))
    except (ProtocolError, asyncio.TimeoutError) as exc:
        print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
