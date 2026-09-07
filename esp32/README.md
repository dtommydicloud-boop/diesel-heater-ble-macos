# ESP32 version (unverified on real hardware)

`heater_control_esp32/heater_control_esp32.ino` ports the same proven
protocol (identical command bytes, GATT UUIDs, checksum) to run on an
ESP32 instead of a Mac/Pi — no Python, no OS, just a few dollars of
microcontroller that can sit permanently plugged in next to the heater.

**Status:** the protocol logic is proven (confirmed live by the Python
version elsewhere in this repo against a real BYD heater). The ESP32 BLE
client code itself has been through two rounds of independent code review
(checked against the real Espressif arduino-esp32 BLE library source, not
just read casually) but has **not yet been flash-tested on real
hardware.** If you try it and it works (or doesn't), please open an issue
or a PR — that's genuinely the missing piece, more than further review.

Two real, separate bugs were caught and fixed across those two review
passes, both before anyone flashed a board:

1. **First pass:** the original draft only scanned once at boot and never
   noticed or recovered from a disconnect — it also matched devices by a
   hardcoded `"byd"`-only name check, which would have silently ignored
   Vevor and other same-protocol units. Fixed to scan/reconnect
   continuously and match primarily by the advertised GATT service UUID.
2. **Second pass** (an independent, later review) found the first pass's
   fix was incomplete: it used the *blocking* scan API by mistake (which
   freezes the reconnect loop and gets permanently stuck after one
   no-match scan), called a client-cleanup function (`deleteClient`) that
   doesn't actually exist in the official Espressif library, and dropped
   the BLE address type when reconnecting. All three are fixed in the
   current version.

Given two rounds of real bugs found this way, treat "unverified on real
hardware" as a genuine, not-yet-closed gap, not a formality — the code has
had careful eyes on it, but nothing here replaces someone actually flashing
a board.

## Board choice matters

Not every board sold as "ESP32" actually has Bluetooth. The original ESP32
and the S3/C3/C6 variants do. The **ESP32-S2 has no Bluetooth at all** (WiFi
only), and the ESP32-P4 has neither WiFi nor Bluetooth built in. Check your
specific board's spec before wiring anything up.

## Flashing

1. Arduino IDE, install the ESP32 board package if you haven't already.
2. Tools → Board → pick your actual ESP32 variant.
3. Open `heater_control_esp32.ino`, upload.
4. Serial Monitor at 115200 baud. Type `on`, `off`, or `status` + Enter.

This is intentionally left as a serial-command demo, same as the Python
version — wiring it to WiFi/MQTT/a web server for real remote control is
the natural next step, and genuinely specific enough to your own setup
that it's left out of this repo on purpose.
