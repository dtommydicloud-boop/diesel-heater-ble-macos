---
name: diesel-heater
description: Remotely start, stop, or check status of Tom's Chinese diesel parking heater (BYD-branded, AirHeaterBLE app protocol) over Bluetooth. Invoke when Tom asks to turn the heater on/off, warm up the cabin remotely, or check the heater's status.
user-invocable: true
allowed-tools:
  - Bash
  - mcp__plugin_telegram_telegram__reply
---

# Diesel Heater Bluetooth Control

Controls Tom's diesel parking heater (BLE name `BYD-...`, matches the AirHeaterBLE app) directly over Bluetooth Low Energy, bypassing the phone app. Real, tested protocol confirmed live 2026-09-06.

## Critical constraint: this only works within Bluetooth range of the heater

The heater is at the lake cabin. This skill only works when run from a machine physically near it (currently: Reed's Lake Mac). If invoked from a machine that isn't in range, dispatch the request to Reed over the Pepin Bus instead of trying to run it locally:

```bash
bash ~/.pepin-labs/bus-send @reed --thread diesel-heater --body "Please turn the heater ON" # or OFF, or STATUS
```

Only run the Bluetooth code directly if this session IS the Lake Mac (or wherever the heater physically is).

## First step, every time: scan by name, never reuse a saved address

BLE addresses are synthetic per observing device/OS — a saved address from a previous run will NOT work on a different run or machine. Always re-scan:

```python
import asyncio
from bleak import BleakScanner

async def find_heater():
    devices = await BleakScanner.discover(timeout=12.0, return_adv=True)
    for addr, (dev, adv) in devices.items():
        name = dev.name or adv.local_name
        if name and 'byd' in name.lower():
            return addr, name
    return None, None

addr, name = asyncio.run(find_heater())
```

**If the scan hangs past ~15 seconds instead of completing or timing out cleanly**: this is a known macOS issue, not a bug in this code. CoreBluetooth silently blocks forever waiting on an unanswered "Terminal wants to use Bluetooth" permission dialog when nobody's at the screen to click it. Kill the process and tell the user (via the Telegram reply tool): *"Bluetooth scan is hanging — someone needs to be at [machine]'s screen to grant the Bluetooth permission popup, or enable it manually in System Settings → Privacy & Security → Bluetooth for Terminal."* Do not just retry silently.

## Protocol (confirmed live, real device responses below)

GATT service: `0000ffe0-0000-1000-8000-00805f9b34fb`
GATT characteristic (write + notify + read): `0000ffe1-0000-1000-8000-00805f9b34fb`

Command frame builder:

```python
def make_cmd(command_type, value):
    data = bytearray([0xAA, 0x55, 0x0C, 0x22, command_type, value, 0x00, 0x00])
    data[-1] = sum(data[2:-1]) % 256
    return data
```

Confirmed command types:
- `0x01` = STATUS (no value needed, e.g. `make_cmd(0x01, 0x00)`) — tested live, returns a real `aa66 01...` response.
- `0x02` = MODE (`0x01`=LEVEL, `0x02`=AUTOMATIC) — not yet live-tested, same proven frame format.
- `0x03` = POWER (`0x01`=ON, `0x00`=OFF) — **tested live both directions.**
  - ON real response: `aa66030100000000000000000000001d0000001e`
  - OFF real response: `aa6603000003a100021500800084001e000000dd`
- `0x04` = LEVEL_OR_TEMP (1-10 in LEVEL mode, 8-36°C in AUTOMATIC mode) — not yet live-tested, same proven frame format.

Send + read a response:

```python
from bleak import BleakClient

CHAR = '0000ffe1-0000-1000-8000-00805f9b34fb'
response_holder = {}

def handler(sender, data):
    response_holder['last'] = bytes(data).hex()

async def send_command(addr, cmd_bytes):
    async with BleakClient(addr, timeout=10.0) as client:
        await client.start_notify(CHAR, handler)
        await client.write_gatt_char(CHAR, cmd_bytes, response=True)
        await asyncio.sleep(3.0)
    return response_holder.get('last')
```

## Putting it together (example: turn heater ON)

```python
import asyncio
addr, name = asyncio.run(find_heater())
if addr is None:
    print("Heater not found in range — check Bluetooth is on and heater is powered.")
else:
    resp = asyncio.run(send_command(addr, make_cmd(0x03, 0x01)))
    print(f"Response: {resp}")
```

## After running any command

Always report back to Tom via the Telegram reply tool what actually happened — the raw hex response alone isn't meaningful to him, translate it plainly ("heater confirmed on" / "no response — might be out of range or off"). Never claim success without a real response frame back from the device.

## Setup dependency (one-time, per machine)

```bash
python3 -m venv /tmp/ble-venv
/tmp/ble-venv/bin/pip install bleak
```

`bleak` (cross-platform BLE library) is required — Bruciatore_BLE's own upstream script uses `bluepy`, which is Linux/BlueZ-only and does not work on macOS. This skill uses `bleak` instead, confirmed working on macOS via CoreBluetooth.

## Source

Real device confirmed 2026-09-06: BLE name `BYD-59951D04886F` (matches AirHeaterBLE app's shortened display `BYD-886F`). Protocol reverse-engineered by the community and referenced from `spin877/Bruciatore_BLE` and `Spettacolo83/homeassistant-diesel-heater` on GitHub (AirHeaterBLE / AA55-AA66 protocol family, shared by a large share of Chinese-made diesel parking heaters regardless of storefront brand).
