# Third-party parts and their licences

This project's own code and pictures are MIT ([LICENSE](LICENSE)). These parts
come from elsewhere and keep their own licences.

## In this repository

| Part | Where | Licence |
|---|---|---|
| M-Flasher's flash loader (`mflr-loader`, from [m-bootloader](https://github.com/jvitkauskas/m-bootloader)) | `src/mflr_loader.h` | MIT, (c) 2026 Julius Vitkauskas |
| The ROM and loader protocol, ported from [M-Flasher](https://github.com/jvitkauskas/m-flasher)'s `jl.rs`, which follows [jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) | `src/jl.cpp`, `src/uboot_host.cpp` | MIT, (c) 2026 Julius Vitkauskas; jl-uboot-tool MIT, (c) 2023 Andrey Grigoryev |
| The restart program (34 bytes), from pi32v2-isa's `tools/hwtest/app/reset.S` as in M-Flasher | `src/jl.cpp` | MIT-0 |
| The rescue images: m-bootloader's boot areas, on a flash header, file table and chip-key record from M-VAVE's packages | `rescue/` | MIT ([rescue/README.md](rescue/README.md), `rescue/LICENSES/`) |
| The guided mode's fonts, rendered from DejaVu Sans and DejaVu Sans Bold | `src/ui_fonts.h` (made by `tools/make_fonts.py`) | Bitstream Vera licence, DejaVu changes public domain ([LICENSES/DejaVu-fonts.txt](LICENSES/DejaVu-fonts.txt)) |

Timings and findings, not code, come from
[FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) and
[flipper-fm1-recovery](https://github.com/merthsoft/flipper-fm1-recovery).

## Linked into the firmware (fetched when building)

| Part | Licence |
|---|---|
| [Arduino-ESP32](https://github.com/espressif/arduino-esp32) core | LGPL-2.1: this project's source and build let anyone rebuild and relink the firmware |
| [ESP-IDF](https://github.com/espressif/esp-idf) and its components, as built into Arduino-ESP32 | Apache-2.0, with the components' own licences |
| [M5Cardputer](https://github.com/m5stack/M5Cardputer), [M5Unified](https://github.com/m5stack/M5Unified), [M5GFX](https://github.com/m5stack/M5GFX) | MIT, (c) M5Stack |
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX), in M5GFX | FreeBSD (BSD-2-Clause), (c) lovyan03 |
| Adafruit GFX's 5x7 font (`Font0`, the advanced mode's console), in M5GFX | BSD, (c) Adafruit Industries |
