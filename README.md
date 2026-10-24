# LUNA OnStepNinja NS-4000

ESP32 telescope mount controller firmware: **OnStep 4.24j** (motor control) and the basic **LUNA AiBridge** features (Wi-Fi, Web pages, ASCOM Alpaca, file storage, LUNA Host Lab connection) running together on one ESP32, plus a **BLE wireless handbox**.

- Firmware version: **v1.1.0** (`:=V#` = `LUNA OnStepNinja NS-4000 v1.1.0`)
- Based on [OnStep](https://github.com/hjd1964/OnStep) 4.24j by Howard Dutton (GPL v3). This is a derived work, **not an official OnStep release**. Please report problems here, not to the OnStep project.
- Compatible with LUNA AiBridge, AiBridge Host and LUNA Host Lab.
- Status: **prototype**. Tested on an NS-3000 board (ESP32, MaxESP3 pin map) with the motors detached from the mount. Not yet tested on a working mount.

## Download

Download **`NS-4000_v<version>_EN.zip`** from the **Releases** page. It contains everything below, including the firmware and handbox binaries (`bin/`, `handbox/`). The repository itself holds the source, the apps and the documents.

## Features (added to OnStep)

- **Wi-Fi** AP + home Wi-Fi (STA) at the same time (replaces Bluetooth Classic). LX200 commands on **TCP 9999 / 9998** (SkySafari, LUNA Host Lab, ASCOM drivers).
- **Web pages**: Console (built in), NS-4000 Control, Settings, star chart.
- **File Manager** (`/files`): upload the apps, register Wi-Fi and a PIN. Open only for **30 minutes after power-on**, then closed.
- **PIN protection**: normally no PIN. Only pages of other web sites need the PIN. AiBridge Host and LUNA Host Lab connect freely.
- **ASCOM Alpaca Telescope** with discovery (UDP 32227): NINA and others connect without a driver. ConformU: 0 errors.
- **Alpaca discovery and relay**: the Console lists the ASCOM Alpaca devices on the LAN; `/api/external/*` relays to them (private LAN addresses only).
- **LUNA Host Lab** (`/mcp`): file and system information tools for AI.
- **GOTO step rate limit** 45,000 pps (recommended 40,000 pps or less). The motor interrupts were made lighter (NS3000_FAST_ISR); tested without stopping up to 60,000 pps.
- **BLE wireless handbox** (ESP32 / ESP32-C3, binaries in `handbox/`): N/S/E/W, Stop, Speed (same as the NS-3000 SEL button). Only one paired handbox connects; stops automatically if the link drops. The Console shows its signal strength and battery voltage.
- **Tracking jitter measurement** (`:SXJ0` / `:GXJ0#`..`:GXJ2#`).

See [COMMANDS.md](COMMANDS.md) for the custom LX200 commands and the Web API.

## Contents

| Folder | |
|---|---|
| `firmware/` | Source code (Arduino sketch `OnStepNinja08d_AiBridge`) - GPL v3 |
| `bin/` | Firmware binaries for flashing, and the flashing command |
| `apps/` | Companion web apps to upload to the NS-4000 - freeware (see `apps/LICENSE`) |
| `handbox/` | BLE handbox binaries to flash (ESP32 / ESP32-C3; no source) - freeware (see `handbox/LICENSE`). `lgpl_relink_*/`: relink kits for the LGPL 2.1 Arduino ESP32 core in the binaries |

## Flashing the binaries

Install esptool (`pip install esptool`), connect the board by USB and run (change `COM6` to your port):

```
python -m esptool --chip esp32 --port COM6 --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size detect 0x1000 OnStepNinja08d_AiBridge.ino.bootloader.bin 0x8000 OnStepNinja08d_AiBridge.ino.partitions.bin 0x10000 OnStepNinja08d_AiBridge.ino.bin
```

## First setup

1. Power on. Connect to the Wi-Fi access point **`OnStepNinja-XXXX`** (no password) and open **http://192.168.4.1/** (the Console).
2. Within 30 minutes, open the **File Manager** (`/files`):
   - enter your home Wi-Fi name and password (and a PIN if you want one) and **Save Config**, then power cycle;
   - upload the apps from `apps/`: `control.html`, `settings.html`, `starchart_en.html`, `starchart_en_data.js`.
3. After the power cycle, the NS-4000 also joins your home Wi-Fi; its address is shown in the Console (Network Information).
4. Open **Settings** once: it sends the date and time if they are not set (OnStep refuses unpark and GOTO until they are set).

## Building from source

- arduino-cli (or Arduino IDE) with **ESP32 Arduino core 2.0.17** (newer cores 3.x do not build this source).
- Library **NimBLE-Arduino 1.4.3** (for the BLE handbox).
- Board: ESP32 Dev Module, Partition Scheme **Huge APP**, Flash Mode DIO, 80 MHz, CPU 240 MHz:
  ```
  arduino-cli compile -b "esp32:esp32:esp32:PartitionScheme=huge_app,FlashMode=dio,FlashFreq=80,CPUFreq=240" firmware/OnStepNinja08d_AiBridge
  ```
- `Config.h`: `NS3000_WIFI_STA_SSID` / `_PASS` (defaults; normally set in the File Manager), `NS3000_WEB_PIN`, `NS3000_BLE`, `NS3000_MAX_STEP_PPS`, `NS3000_FAST_ISR`. The mount settings are the original OnStep ones.

## Safety

This firmware and the apps move a telescope mount. Stay with the equipment and be ready to cut the power. Write down the existing settings before changing them. The software is provided without warranty.

## Licenses

- **Firmware** (`firmware/`, including the built-in Console and File Manager): **GNU GPL v3** (see `firmware/LICENSE`), like OnStep. Copyright of the OnStep code: Howard Dutton and contributors.
- **Apps** (`apps/`) and **handbox binaries** (`handbox/`): **freeware**, Copyright (c) 2026 Nishioka Sadahiko. Personal use is free; giving copies to friends at cost is allowed; selling, commercial use and re-hosting are not allowed. See the LICENSE in each folder.
- Third-party parts: d3-celestial star data (BSD 3-Clause), NimBLE-Arduino (Apache 2.0), arduino-esp32 (LGPL 2.1), ESP-IDF (Apache 2.0) - see the notices in `apps/` and `handbox/`.

Repository: https://github.com/OnStepNinja/OnStepNinja-NS4000  -  LUNA Project: https://github.com/OnStepNinja
