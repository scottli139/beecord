#!/usr/bin/env python3
"""BuzzLink BLE infrastructure smoke test (firmware M4.1).

Verifies the device-side BLE bring-up end to end with bleak:

    1. scan for an advertiser whose name starts with BCD- (preferring one that
       advertises the BuzzLink service UUID);
    2. connect and read the negotiated MTU;
    3. discover the BuzzLink service and both characteristics
       (Cmd Write = write-without-response, Event Notify = notify);
    4. write the BUZZLINK.md section 11 V1 PING test frame to Cmd Write
       (the firmware M4.1 write callback logs a hex dump; no reply yet);
    5. subscribe to Event Notify, then unsubscribe
       (the firmware logs SUBSCRIBE/MTU events during the whole run).

Every check prints PASS/FAIL immediately; the summary line and the exit code
(0 = all checks passed, 1 = at least one failed) follow at the end.

Usage:
    python3 ble_smoke.py [--timeout SECONDS] [--name-prefix PREFIX]

Requires bleak (pip install bleak). On macOS the terminal needs Bluetooth
permission (System Settings > Privacy & Security > Bluetooth).
"""

from __future__ import annotations

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

# GATT definition, BUZZLINK.md section 2.1 (canonical text form; bleak lowercases).
SERVICE_UUID = "0E242009-7EF4-4AE0-B3B0-E3A59E58D47C"
CMD_WRITE_UUID = "A2268D42-7024-430F-B059-9A156436973D"
EVENT_NOTIFY_UUID = "C22080F5-0E96-4823-8909-1DD2CE427D2D"

# Section 11 V1 PING: type=0x01 session=0x2A channel=0x0000, empty payload.
PING_FRAME = bytes.fromhex("beec01012a0000000000867d")


class CheckLog:
    """Running PASS/FAIL tally; each check prints immediately."""

    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, msg: str) -> None:
        self.passed += 1
        print(f"PASS  {msg}")

    def fail(self, msg: str) -> None:
        self.failed += 1
        print(f"FAIL  {msg}")

    def summary(self, label: str) -> int:
        total = self.passed + self.failed
        verdict = "PASS" if self.failed == 0 else "FAIL"
        print(f"\nSMOKE {verdict}: {label} — {self.passed}/{total} checks passed")
        return 0 if self.failed == 0 else 1


async def scan_for_device(log: CheckLog, prefix: str, timeout: float) -> BLEDevice | None:
    """Scan and return the best BCD-* candidate, preferring a BuzzLink-service advertiser."""
    print(f"scanning for devices named {prefix}* ({timeout:.0f}s) ...")
    adv_map = await BleakScanner.discover(timeout=timeout, return_adv=True)

    best: BLEDevice | None = None
    for device, adv in adv_map.values():
        name = adv.local_name or device.name or ""
        if not name.startswith(prefix):
            continue
        uuids = [u.lower() for u in adv.service_uuids]
        has_svc = SERVICE_UUID.lower() in uuids
        print(f"  candidate {name} ({device.address}) service-uuid-in-adv={has_svc}")
        # First match wins; a candidate advertising our service UUID always wins.
        if best is None or has_svc:
            best = device
            if has_svc:
                break
    return best


async def run(args: argparse.Namespace) -> int:
    log = CheckLog()

    device = await scan_for_device(log, args.name_prefix, args.timeout)
    if device is None:
        log.fail(f"no device named {args.name_prefix}* found")
        return log.summary("scan")
    log.ok(f"found {device.name} ({device.address})")

    client = BleakClient(device, timeout=args.timeout)
    try:
        await client.connect()
    except Exception as exc:  # bleak raises platform-specific subclasses
        log.fail(f"connect: {exc!r}")
        return log.summary("connect")

    try:
        log.ok(f"connected (mtu={client.mtu_size})")

        services = {svc.uuid.lower(): svc for svc in client.services}
        svc = services.get(SERVICE_UUID.lower())
        if svc is None:
            log.fail(f"service {SERVICE_UUID} not discovered")
            return log.summary("discover")
        log.ok(f"service {SERVICE_UUID}")

        chars = {chr_.uuid.lower(): chr_ for chr_ in svc.characteristics}
        cmd = chars.get(CMD_WRITE_UUID.lower())
        notify = chars.get(EVENT_NOTIFY_UUID.lower())
        if cmd is None:
            log.fail(f"Cmd Write characteristic {CMD_WRITE_UUID} not discovered")
        else:
            log.ok(f"Cmd Write {CMD_WRITE_UUID} properties={sorted(cmd.properties)}")
            if "write-without-response" not in cmd.properties:
                log.fail("Cmd Write lacks write-without-response property")
            else:
                log.ok("Cmd Write has write-without-response property")
        if notify is None:
            log.fail(f"Event Notify characteristic {EVENT_NOTIFY_UUID} not discovered")
        else:
            log.ok(f"Event Notify {EVENT_NOTIFY_UUID} properties={sorted(notify.properties)}")
            if "notify" not in notify.properties:
                log.fail("Event Notify lacks notify property")
            else:
                log.ok("Event Notify has notify property")
        if cmd is None or notify is None:
            return log.summary("discover")

        # Firmware M4.1 logs the hex dump of this frame; no PONG is expected yet.
        try:
            await client.write_gatt_char(CMD_WRITE_UUID, PING_FRAME, response=False)
            log.ok(f"wrote V1 PING frame ({len(PING_FRAME)}B) to Cmd Write")
        except Exception as exc:
            log.fail(f"write to Cmd Write: {exc!r}")

        # Subscribe/unsubscribe; firmware logs SUBSCRIBE events (M4.1 sends nothing).
        received = 0

        def _on_notify(_char, _data: bytearray) -> None:
            nonlocal received
            received += 1

        try:
            await client.start_notify(EVENT_NOTIFY_UUID, _on_notify)
            log.ok("subscribed Event Notify")
            await asyncio.sleep(0.5)  # let the subscription settle on the device
            await client.stop_notify(EVENT_NOTIFY_UUID)
            log.ok("unsubscribed Event Notify")
            if received:
                log.ok(f"received {received} notify(ies) while subscribed")
            else:
                print("INFO  no notify data during 0.5s (expected in M4.1: device sends nothing)")
        except Exception as exc:
            log.fail(f"notify subscribe/unsubscribe: {exc!r}")
    finally:
        if client.is_connected:
            await client.disconnect()
        print("disconnected")

    return log.summary("full flow")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="scan/connect timeout in seconds (default: 10)")
    parser.add_argument("--name-prefix", default="BCD-",
                        help="expected device name prefix (default: BCD-)")
    args = parser.parse_args(argv[1:])
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main(sys.argv))
