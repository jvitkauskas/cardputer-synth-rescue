#include "writer.h"

#include <stdio.h>
#include <string.h>

#include "jl.h"
#include "rescue_info.h"

extern "C" const uint8_t rescue_fm1_boot[], rescue_fm1_boot_end[], rescue_fm1_app[], rescue_fm1_app_end[];
extern "C" const uint8_t rescue_smk37_boot[], rescue_smk37_boot_end[], rescue_smk37_app[], rescue_smk37_app_end[];

namespace writer {

static constexpr uint32_t BOOT_AREA = 0x4000;
static constexpr uint32_t FLASH_SIZE = 0x100000;
static uint8_t blank[SECTOR];
static uint8_t sector_buf[SECTOR];

const Image &image(Board b) {
    static const Image fm1 = {
        "FM-1", rescue_fm1_boot, rescue_fm1_app,
        (uint32_t)(rescue_fm1_boot_end - rescue_fm1_boot), (uint32_t)(rescue_fm1_app_end - rescue_fm1_app),
        RESCUE_FM1_BOOT_CRC32, RESCUE_FM1_APP_CRC32, 0, 0,
    };
    // BTIF (Bluetooth address, pairings) and USRTRIM (the factory calibration)
    static const Image smk37 = {
        "SMK-37", rescue_smk37_boot, rescue_smk37_app,
        (uint32_t)(rescue_smk37_boot_end - rescue_smk37_boot), (uint32_t)(rescue_smk37_app_end - rescue_smk37_app),
        RESCUE_SMK37_BOOT_CRC32, RESCUE_SMK37_APP_CRC32, 0xC0000, 0xC2000,
    };
    return b == SMK37 ? smk37 : fm1;
}

static bool is_protected(const Image &img, uint32_t s) {
    uint32_t a = s * SECTOR;
    return a >= END || (a >= img.keep_lo && a < img.keep_hi);
}

// The image's bytes for sector s: boot area, app area, then erased flash.
static const uint8_t *target(const Image &img, uint32_t s) {
    uint32_t a = s * SECTOR;
    if (a < BOOT_AREA) return img.boot + a;
    if (a < BOOT_AREA + img.app_len) return img.app + (a - BOOT_AREA);
    if (!blank[0]) memset(blank, 0xFF, sizeof blank);
    return blank;
}

static bool is_blank(const uint8_t *d) {
    for (uint32_t i = 0; i < SECTOR; i++)
        if (d[i] != 0xFF) return false;
    return true;
}

static uint32_t target_crc(const Image &img, uint32_t first, uint32_t count) {
    uint32_t crc = 0;
    for (uint32_t s = first; s < first + count; s++) crc = jl::crc32_update(crc, target(img, s), SECTOR);
    return crc;
}

bool image_ok(const Image &img, char *err, size_t errlen) {
    uint32_t app_end = BOOT_AREA + img.app_len;
    if (img.boot_len != BOOT_AREA || img.app_len % SECTOR || app_end > END ||
        (img.keep_hi > img.keep_lo && app_end > img.keep_lo)) {
        snprintf(err, errlen, "%s rescue image sizes are wrong (%lu, %lu)", img.name, (unsigned long)img.boot_len,
                 (unsigned long)img.app_len);
        return false;
    }
    if (jl::crc32_update(0, img.boot, img.boot_len) != img.boot_crc ||
        jl::crc32_update(0, img.app, img.app_len) != img.app_crc) {
        snprintf(err, errlen, "the %s rescue image in this app is damaged: reinstall the app", img.name);
        return false;
    }
    return true;
}

bool plan(const Image &img, Plan *p, Progress progress, char *err, size_t errlen) {
    memset(p, 0, sizeof *p);
    bool prev = false;
    for (uint32_t s = 0; s < SECTORS; s++) {
        bool change = false;
        if (!is_protected(img, s)) {
            if (!jl::read(s * SECTOR, sector_buf, SECTOR, err, errlen)) return false;
            change = memcmp(sector_buf, target(img, s), SECTOR) != 0;
        }
        p->change[s] = change;
        if (change) {
            if (is_blank(target(img, s))) p->to_erase++;
            else p->to_write++;
            if (!prev) p->runs++;
        }
        prev = change;
        if (s % 16 == 15) progress(s + 1, SECTORS);
    }
    return true;
}

// One run of changed sectors: erase, program, verify with the chip's CRC32.
static bool do_run(const Image &img, uint32_t first, uint32_t count, char *err, size_t errlen) {
    if (!jl::erase(first * SECTOR, count * SECTOR, err, errlen)) return false;
    for (uint32_t s = first; s < first + count; s++) {
        const uint8_t *t = target(img, s);
        if (!is_blank(t) && !jl::program(s * SECTOR, t, SECTOR, err, errlen)) return false;
    }
    uint32_t chip;
    if (!jl::crc32(first * SECTOR, count * SECTOR, &chip, err, errlen)) return false;
    uint32_t want = target_crc(img, first, count);
    if (chip != want) {
        snprintf(err, errlen, "0x%05lX-0x%05lX reads back CRC %08lX, not %08lX", (unsigned long)(first * SECTOR),
                 (unsigned long)((first + count) * SECTOR - 1), (unsigned long)chip, (unsigned long)want);
        return false;
    }
    return true;
}

// [lo, hi) on the chip against the image.
static bool check_written(const Image &img, uint32_t lo, uint32_t hi, char *err, size_t errlen) {
    uint32_t chip;
    if (!jl::crc32(lo, hi - lo, &chip, err, errlen)) return false;
    if (chip != target_crc(img, lo / SECTOR, (hi - lo) / SECTOR)) {
        snprintf(err, errlen, "final check: 0x%05lX-0x%05lX does not match the image", (unsigned long)lo,
                 (unsigned long)(hi - 1));
        return false;
    }
    return true;
}

bool apply(const Image &img, const Plan &p, Log log, Progress progress, char *err, size_t errlen) {
    // the protected ranges as they are now: they must read the same afterwards
    bool keep = img.keep_hi > img.keep_lo;
    uint32_t kept_crc = 0, top_crc = 0;
    if ((keep && !jl::crc32(img.keep_lo, img.keep_hi - img.keep_lo, &kept_crc, err, errlen)) ||
        !jl::crc32(END, FLASH_SIZE - END, &top_crc, err, errlen))
        return false;

    uint32_t total = p.to_write + p.to_erase, done = 0;
    progress(0, total);
    for (uint32_t s = 0; s < SECTORS;) {
        if (!p.change[s]) {
            s++;
            continue;
        }
        uint32_t first = s;
        while (s < SECTORS && p.change[s]) s++;
        uint32_t count = s - first;
        log("0x%05lX-0x%05lX: %lu sector%s", (unsigned long)(first * SECTOR), (unsigned long)(s * SECTOR - 1),
            (unsigned long)count, count == 1 ? "" : "s");
        if (!do_run(img, first, count, err, errlen)) {
            log("Not verified (%s): once more", err);
            if (!do_run(img, first, count, err, errlen)) return false;
        }
        done += count;
        progress(done, total);
    }

    if (keep) {
        if (!check_written(img, 0, img.keep_lo, err, errlen) || !check_written(img, img.keep_hi, END, err, errlen))
            return false;
    } else if (!check_written(img, 0, END, err, errlen)) {
        return false;
    }
    uint32_t crc;
    if (keep) {
        if (!jl::crc32(img.keep_lo, img.keep_hi - img.keep_lo, &crc, err, errlen)) return false;
        if (crc != kept_crc) {
            snprintf(err, errlen, "final check: protected 0x%05lX-0x%05lX changed", (unsigned long)img.keep_lo,
                     (unsigned long)(img.keep_hi - 1));
            return false;
        }
    }
    if (!jl::crc32(END, FLASH_SIZE - END, &crc, err, errlen)) return false;
    if (crc != top_crc) {
        snprintf(err, errlen, "final check: protected 0x%05lX- changed", (unsigned long)END);
        return false;
    }
    if (keep)
        log("Verified: as the image, 0x%05lX-0x%05lX and 0x%05lX- unchanged", (unsigned long)img.keep_lo,
            (unsigned long)(img.keep_hi - 1), (unsigned long)END);
    else
        log("Verified: 0x00000-0x%05lX as the image, 0x%05lX- unchanged", (unsigned long)(END - 1), (unsigned long)END);
    return true;
}

}  // namespace writer
