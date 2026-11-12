# Rescue images: FM-1 and SMK-37

What the Cardputer writes with **W** (`src/writer.cpp`): a clean install of a
bootloader and a minimal open firmware, so a bricked unit starts again and can
take any other firmware afterwards over USB MIDI (web installers, M-UPGRADE,
M-Flasher), with no computer needed for the rescue itself. The app picks the
image for the unit it identified.

## FM-1 (`fm1/`)

| Range | Contents |
|---|---|
| `0x00000`-`0x03FFF` | `fm1/boot.bin`: [m-bootloader](https://github.com/jvitkauskas/m-bootloader) 2.3-6bc8f45 for the FM-1 (as bundled in [M-Flasher](https://github.com/jvitkauskas/m-flasher) 0.11.0), installed the way M-Flasher installs it |
| `0x04000`-`0x92FFF` | `fm1/app.bin`: [fm1-base](https://github.com/charlesvestal/fm1-base)'s template (`e35f7e2`, felucca-bsp `33cac69`), built with `tools/build.py template --no-ble`; identity `FM-1_1100` |
| `0x93000`-`0xFEFFF` | erased: whatever earlier firmwares left there (presets, settings, data) goes |
| `0xFF000`-`0xFFFFF` | never written (key_mac: factory Bluetooth address, where present) |

The template: a single synth voice on the keys (OCT-/OCT+ octave, PRESET wave,
MASTER volume), USB MIDI both ways, M-VAVE's USB MIDI update protocol, and the
ROM download mode from USB MIDI or by holding OCT- and OCT+ for 5 s.
m-bootloader adds its own way in: hold REC while switching on.

| File | Bytes | CRC32 | SHA-256 |
|---|---|---|---|
| `fm1/boot.bin` | 16384 | `A29F263D` | `e8f8046985c620493f59054a50a3330547ad7103b08bf97f8e42a284d92a694e` |
| `fm1/app.bin` | 585728 | `6D827B25` | `33b1bbc61ea1bd6664a3253d3b60eb41f28d9c738bb6b8d39ddd6ae34dbdab02` |

## SMK-37 Pro / Elite / MKE-P37 (`smk37/`)

The same hardware under three names: one image for all, the Pro's.

| Range | Contents |
|---|---|
| `0x00000`-`0x03FFF` | `smk37/boot.bin`: m-bootloader 2.3-6bc8f45 for the SMK-37, on the header, file table and chip-key record of the official SMK-37 Pro 016 package |
| `0x04000`-`0x9CFFF` | `smk37/app.bin`: fm1-base's template from its `smk37-inhouse` branch (`eafac16`, felucca-bsp `a702b84`), built with `FM1B_BOARD=smk37 FM1B_MODEL=pro tools/build.py template --no-ble`; identity `SMK-37 Pro_1100` |
| `0x9D000`-`0xBFFFF`, `0xC2000`-`0xFEFFF` | erased |
| `0xC0000`-`0xC1FFF` | never written: BTIF (Bluetooth address, pairings) and USRTRIM (the factory key, pad and wheel calibration) |
| `0xFF000`-`0xFFFFF` | never written: key_mac, and m-bootloader's copy of the calibration (TRIMCOPY) |

fm1-base's SMK-37 layer is marked "blind" there (built, its flash map checked
against these protected ranges, its simulator run); this image has since
rescued an SMK-37 Pro.

| File | Bytes | CRC32 | SHA-256 |
|---|---|---|---|
| `smk37/boot.bin` | 16384 | `029D5A23` | `6a96db9899286868372e4853069439192e7cf766d09b6857574c52b72fefc7a5` |
| `smk37/app.bin` | 626688 | `9697FA5F` | `138ce6a8e1ec4feda80674f50d902b700c7a0b35525692f86449aaedd87c0706` |

## How it is made

`tools/make_rescue_image.sh BOARD M-FLASHER-SRC TEMPLATE.fwsc OFFICIAL.fwsc`
runs M-Flasher's own code (`tools/rescuegen.rs` in a scratch copy of its
source):

1. The template package's `flash.bin` (the app area, already encrypted with
   the chip key `980F` every FM-1 uses) over erased flash.
2. The boot area: m-bootloader installed with M-Flasher's
   `build_short_boot_area` (m-bootloader 1.4 and newer reads no unit
   configuration), on the flash header, file table and chip-key record of an
   official M-VAVE package (FM-1 015, SMK-37 Pro 016). fm1-base's packages carry only a
   stand-in boot area, with a different chip-key record.
3. Checked: the app files pass their CRCs under key `980F`, and M-Flasher
   reads the result as m-bootloader 2.3 with firmware "FM-1 110" /
   "SMK-37 Pro 110".

Each boot area is byte-identical to the one M-Flasher left on a real unit
(an FM-1, an SMK-37 Pro) when it installed m-bootloader 2.3 there.

## Licences

- m-bootloader: MIT (`LICENSES/MIT-m-bootloader.txt`).
- fm1-base and felucca-bsp: MPL-2.0 (`LICENSES/MPL-2.0.txt`); source at the
  commits above.
- In the app area, JieLi AC79 SDK files `cfg_tool.bin` and `eq_cfg_hw.bin`:
  Apache-2.0 (`LICENSES/Apache-2.0.txt`), Copyright (c) JIELI (Zhuhai Jieli
  Technology Co., Ltd.).
- Fonts in the template: Barlow Semi Condensed and Terminus, SIL OFL 1.1
  (`LICENSES/OFL-1.1-*.txt`).
- The boot area's 160-byte flash header and file table and the 34-byte
  chip-key record come from M-VAVE's FM-1 015 / SMK-37 Pro 016 packages, as
  every installer that puts m-bootloader on a unit keeps them.
