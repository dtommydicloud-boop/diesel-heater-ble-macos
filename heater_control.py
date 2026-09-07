#!/usr/bin/env python3
"""
Control a Chinese diesel parking heater (BYD / AirHeaterBLE-app protocol)
over Bluetooth Low Energy, from macOS.

Why this exists: the existing community reverse-engineering (e.g.
spin877/Bruciatore_BLE, Spettacolo83/homeassistant-diesel-heater) is built
on `bluepy`, which only works on Linux via BlueZ. This is a port to
`bleak` (cross-platform), confirmed working on macOS via CoreBluetooth.

Usage:
    pip install bleak
    python3 heater_control.py on
    python3 heater_control.py off
    python3 heater_control.py status

The heater must already be powered (plugged into 12V) and within Bluetooth
range. This does NOT replace the physical fuel/power system -- it only
sends the same commands the AirHeaterBLE phone app sends.
"""

import asyncio
import sys

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"
CHAR_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"
SCAN_TIMEOUT_S = 12.0
COMMAND_TIMEOUT_S = 10.0

# Advertised-name substrings seen in the wild for this protocol. This is a
# convenience/logging aid, NOT the real match -- name-only matching would
# silently miss any brand not in this list (a real bug found in an earlier
# version: it only checked for "byd", so a Vevor-branded unit using the
# identical protocol would never be found despite the README claiming
# Vevor compatibility). The real match is the advertised GATT service UUID,
# which is protocol-specific regardless of what brand name is on the box.
KNOWN_NAME_HINTS = ("byd", "vevor", "airheater")

# Confirmed command types (byte 4 of the frame):
POWER = 0x03      # value: 0x01=ON, 0x00=OFF
STATUS = 0x01     # value: 0x00 (no-op value, status is a query)
MODE = 0x02       # value: 0x01=LEVEL, 0x02=AUTOMATIC (not yet exercised by this script)
LEVEL_OR_TEMP = 0x04  # value: 1-10 in LEVEL mode, 8-36 in AUTOMATIC mode (not yet exercised)


def make_cmd(command_type: int, value: int) -> bytearray:
    """Build a command frame. Checksum is a simple sum-mod-256 over bytes 2..-2."""
    data = bytearray([0xAA, 0x55, 0x0C, 0x22, command_type, value, 0x00, 0x00])
    data[-1] = sum(data[2:-1]) % 256
    return data


async def find_heater() -> tuple[str | None, str | None]:
    """Scan for the heater by its advertised GATT service UUID -- the real,
    protocol-level identifier, not the brand name printed on the box (which
    varies: BYD, Vevor, and others all use this identical protocol). Never
    reuse a saved address either -- CoreBluetooth (and most BLE stacks)
    assign a synthetic address per observing device/OS, so a saved address
    from a different machine or even a previous boot will not necessarily
    work."""
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT_S, return_adv=True)
    for addr, (dev, adv) in devices.items():
        name = dev.name or adv.local_name
        service_uuids = [u.lower() for u in (adv.service_uuids or [])]
        if SERVICE_UUID in service_uuids:
            return addr, name
        # Fallback for devices that don't advertise the service UUID
        # directly (some BLE stacks only expose it after connecting):
        # match on a known name substring instead.
        if name and any(hint in name.lower() for hint in KNOWN_NAME_HINTS):
            return addr, name
    return None, None


async def send_command(address: str, cmd_bytes: bytearray) -> str | None:
    response: dict[str, str] = {}

    def handler(_sender, data):
        response["last"] = bytes(data).hex()

    async with BleakClient(address, timeout=COMMAND_TIMEOUT_S) as client:
        await client.start_notify(CHAR_UUID, handler)
        await client.write_gatt_char(CHAR_UUID, cmd_bytes, response=True)
        await asyncio.sleep(3.0)
    return response.get("last")


async def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in ("on", "off", "status"):
        print(f"Usage: {sys.argv[0]} <on|off|status>", file=sys.stderr)
        return 2

    print("Scanning for heater (AirHeaterBLE protocol service)...")
    addr, name = await find_heater()
    if addr is None:
        print(
            "Heater not found. If this hangs instead of finishing in "
            f"~{SCAN_TIMEOUT_S:.0f}s, macOS is likely blocking on an unanswered "
            "Bluetooth permission prompt -- check System Settings -> "
            "Privacy & Security -> Bluetooth for this terminal/app, or "
            "click the popup if one's on screen.",
            file=sys.stderr,
        )
        return 1
    print(f"Found: {name} ({addr})")

    action = sys.argv[1]
    if action == "on":
        cmd = make_cmd(POWER, 0x01)
    elif action == "off":
        cmd = make_cmd(POWER, 0x00)
    else:
        cmd = make_cmd(STATUS, 0x00)

    resp = await send_command(addr, cmd)
    if resp is None:
        print("No response received -- command may not have taken effect.")
        return 1
    print(f"Response: {resp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
