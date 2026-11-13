// Installs a rescue image (m-bootloader's boot area, see rescue/README.md)
// with M-Flasher's rules, plus a clean install: every writable sector below
// 0xFF000 ends up exactly as the image says, the boot area written,
// everything else erased (no firmware, no leftovers of earlier ones), so
// m-bootloader opens its MIDI mode at the next start and waits for a firmware. Never touched: 0xFF000-0xFFFFF (key_mac, and on the
// SMK-37 m-bootloader's copy of the calibration) and, on the SMK-37,
// 0xC0000-0xC1FFF (Bluetooth pairing, the factory calibration).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace writer {

static constexpr uint32_t SECTOR = 0x1000;
static constexpr uint32_t END = 0xFF000;  // first byte never written
static constexpr uint32_t SECTORS = END / SECTOR;

typedef void (*Log)(const char *fmt, ...);
typedef void (*Progress)(uint32_t done, uint32_t total);

enum Board { FM1, SMK37 };

struct Image {
    const char *name;            // for the screen
    const uint8_t *boot;         // 0x00000-0x03FFF
    uint32_t boot_len;           // as embedded
    uint32_t boot_crc;           // as built (src/rescue_info.h)
    uint32_t keep_lo, keep_hi;   // a protected range below END (empty: equal)
};

const Image &image(Board b);

// The embedded boot area is intact (their CRC32s match the build's).
bool image_ok(const Image &img, char *err, size_t errlen);

struct Plan {
    bool change[SECTORS];  // sector differs from the image (never a protected one)
    uint32_t to_write;     // changed sectors that hold data in the image
    uint32_t to_erase;     // changed sectors that are blank in the image
    uint32_t runs;
};

// Reads the unit's flash sector by sector and compares it with the image
// (with or without a backup on the SD card).
bool plan(const Image &img, Plan *p, Progress progress, char *err, size_t errlen);

// Notes the chip's CRC32 of every protected range, then erases, programs and
// verifies run by run (a run that does not verify is done once more, then the
// write stops), then checks every written range against the image and every
// protected one against its CRC from before.
bool apply(const Image &img, const Plan &p, Log log, Progress progress, char *err, size_t errlen);

}  // namespace writer
