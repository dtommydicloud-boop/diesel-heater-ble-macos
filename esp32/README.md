# ESP32 version (unverified on real hardware)

`heater_control_esp32/heater_control_esp32.ino` ports the same proven
protocol (identical command bytes, GATT UUIDs, checksum) to run on an
ESP32 instead of a Mac/Pi — no Python, no OS, just a few dollars of
microcontroller that can sit permanently plugged in next to the heater.

**Status:** the protocol logic is proven (confirmed live by the Python
version elsewhere in this repo against a real BYD heater). The ESP32 BLE
client code itself has not yet been flash-tested on real hardware. If you
try it and it works (or doesn't), please open an issue or a PR.

This version scans/reconnects continuously in the background rather than
giving up after one attempt — a real gap an earlier version had, caught by
code review before anyone flashed it: the first draft only scanned once at
boot and never noticed or recovered from a disconnect, which defeats the
entire point of using this as an always-on bridge. It also matches devices
by the advertised GATT service UUID (not just a hardcoded "byd" name
check), so Vevor and other same-protocol brands aren't silently ignored.

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
