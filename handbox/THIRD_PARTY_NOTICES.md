# Third-party components in the BleHandBox binaries

The `.bin` files of the handbox (`bin_esp32/` and `bin_esp32c3/`) contain the following components, each under its own license. The author's license in `LICENSE` applies only to the handbox program itself.

| Component | License | Source |
|---|---|---|
| NimBLE-Arduino 1.4.3 (h2zero), incl. Apache Mynewt NimBLE | Apache License 2.0 | https://github.com/h2zero/NimBLE-Arduino |
| arduino-esp32 2.0.17 (Espressif) | LGPL 2.1 | https://github.com/espressif/arduino-esp32 |
| ESP-IDF (Espressif) | Apache License 2.0 | https://github.com/espressif/esp-idf |

- Apache 2.0: the license text is in `lgpl_relink_*/licenses/Apache-2.0.txt`; copyright notices are kept; no warranty.
- LGPL 2.1 (arduino-esp32): the handbox binaries are distributed without the sketch source. As LGPL 2.1 section 6a requires, each binary is accompanied by a relink kit (`lgpl_relink_esp32/`, `lgpl_relink_esp32c3/`): the object files of the handbox program and of NimBLE-Arduino, the link command, `relink.py`, the source of the core (arduino-esp32 2.0.17) and the license text (`licenses/LGPL-2.1.txt`). See `README.txt` in the kit.
- The NS-4000 firmware (OnStep 4.24j based, GPL v3) is a separate work; see its own source and notices.
