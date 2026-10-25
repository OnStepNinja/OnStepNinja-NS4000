# BLE wireless handbox

The handbox program is distributed as **binaries only** (no source code), in the Release zip `NS-4000_v1.1.1_EN.zip`:

- `handbox/bin_esp32/`, `handbox/bin_esp32c3/` - binaries and the flashing command for ESP32 and ESP32-C3
- `handbox/lgpl_relink_esp32/`, `handbox/lgpl_relink_esp32c3/` - relink kits for the Arduino ESP32 core (LGPL 2.1) contained in the binaries (see THIRD_PARTY_NOTICES.md)

Pins (button to GND): ESP32 - N 25, S 26, E 27, W 32, Stop 33, Speed 14, LED 2 (active high), battery voltage 34 (1M/1M divider, 0.1uF).
ESP32-C3 - N 0, S 1, E 3, W 4, Stop 5, Speed 6, LED 8 (active low).
Turn the handbox on within 30 minutes after powering on the NS-4000 to pair (register) it. See COMMANDS.md for the protocol.
