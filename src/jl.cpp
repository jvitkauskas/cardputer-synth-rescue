// Port of M-Flasher's src/jl.rs (MIT, after jl-uboot-tool by kagaimiq, MIT).
// Commands are 16-byte CDBs: a 16-bit opcode, big-endian arguments, padded
// with 0xFF. Loader replies are 16 bytes with the opcode echoed in bytes 0-1.
#include "jl.h"

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mflr_loader.h"
#include "uboot_host.h"

namespace jl {

static constexpr uint16_t IDENTIFY = 0xFC40;
static constexpr uint16_t CRC32 = 0xFC41;
static constexpr uint16_t UNIQUE_ID = 0xFC42;
static constexpr uint16_t LAST_ERROR = 0xFC46;
static constexpr uint8_t OUT_OF_RANGE = 3;
static constexpr uint16_t ERASE_BLOCK = 0xFB00;
static constexpr uint16_t ERASE_SECTOR = 0xFB01;
static constexpr uint16_t WRITE_FLASH = 0xFB04;
static constexpr uint16_t WRITE_MEMORY = 0xFB06;
static constexpr uint16_t JUMP = 0xFB08;
static constexpr uint16_t READ_FLASH = 0xFD05;
static constexpr uint16_t READ_KEY = 0xFC09;
static constexpr uint16_t ONLINE_DEVICE = 0xFC0A;
static constexpr uint16_t USB_BUFFER_SIZE = 0xFC14;
static constexpr uint16_t LOADER_ARG = 1;  // target: SPI NOR flash
static constexpr int RETRIES = 4;
static constexpr uint32_t READ_IO = 16384;

// M-Flasher's RESTART program: clears the download-mode marker at 0x01C7FD80
// and requests a software reset (pi32v2-isa tools/hwtest/app/reset.S, MIT-0).
static const uint8_t RESTART_PROG[34] = {
    0xc4, 0xff, 0x80, 0xfd, 0xc7, 0x01, 0x41, 0x20, 0xc1, 0x60, 0xc1, 0x61, 0xc1, 0x62, 0xc1, 0x63,
    0x20, 0x00, 0x60, 0xe0, 0x80, 0x4b, 0x41, 0x60, 0x42, 0x30, 0x21, 0x19, 0xc1, 0x60, 0x20, 0x00,
    0xf7, 0x9f,
};

static Info *cur;
static uint32_t running_at;  // address of our running loader

// ---------------------------------------------------------------- checksums

uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *d, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void crc_cipher(const uint8_t *in, uint8_t *out, size_t n, uint32_t key) {
    static const uint8_t MAGIC[16] = {0xC3, 0xCF, 0xC0, 0xE8, 0xCE, 0xD2, 0xB0, 0xAE,
                                      0xC4, 0xE3, 0xA3, 0xAC, 0xD3, 0xF1, 0xC1, 0xD6};
    uint8_t hi[2] = {(uint8_t)(key >> 16), (uint8_t)(key >> 24)};  // (key >> 16) as LE u16
    uint16_t c = crc16_update(key & 0xFFFF, hi, 2);
    for (size_t i = 0; i < n; i++) {
        c = crc16_update(c, &MAGIC[i % 16], 1);
        out[i] = in[i] ^ (uint8_t)c;
    }
}

// ---------------------------------------------------------------- commands

static void cdb(uint8_t out[16], uint16_t cmd, const uint8_t *args, size_t n) {
    memset(out, 0xFF, 16);
    out[0] = cmd >> 8;
    out[1] = cmd;
    memcpy(out + 2, args, n);
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// A command tried up to RETRIES times (failed or refused, as M-Flasher); with
// refusal_answers a refusal is the answer and is not sent again.
static host::Result attempts(const uint8_t c[16], uint8_t *buf, uint32_t len, bool in, uint32_t *moved,
                             bool refusal_answers, char *err, size_t errlen) {
    host::Result r = host::FAILED;
    for (int attempt = 1; attempt <= RETRIES; attempt++) {
        r = host::execute(c, 16, buf, len, in, moved, err, errlen);
        if (r == host::OK) return r;
        if (r == host::REFUSED && refusal_answers) return r;
        if (!host::connected()) return host::FAILED;
        if (attempt < RETRIES) {
            if (cur) cur->retries++;
            vTaskDelay(pdMS_TO_TICKS(250 * attempt));
        }
    }
    return r;
}

// A command without data; resp gets the 16 answer bytes (echo included).
static host::Result call16(uint16_t cmd, const uint8_t *args, size_t n, uint8_t resp[16], bool probe, char *err,
                           size_t errlen) {
    uint8_t c[16];
    uint32_t moved;
    cdb(c, cmd, args, n);
    memset(resp, 0, 16);
    host::Result r = attempts(c, resp, 16, true, &moved, probe, err, errlen);
    if (r != host::OK) return r;
    if (((resp[0] << 8) | resp[1]) != cmd) {
        snprintf(err, errlen, "unexpected answer to command 0x%04X", cmd);
        return probe ? host::REFUSED : host::FAILED;
    }
    return host::OK;
}

// Our loader's version and address, if it is the one answering.
static bool identify(char ver[8], uint32_t *at, uint8_t *mhz) {
    uint8_t r[16];
    char e[64];
    if (call16(IDENTIFY, nullptr, 0, r, true, e, sizeof e) != host::OK) return false;
    if (memcmp(r + 2, "MFLR", 4) != 0) return false;
    memcpy(ver, r + 6, 3);
    ver[3] = 0;
    for (int i = 2; i >= 0 && ver[i] == 0; i--) ver[i] = 0;
    *at = rd_be32(r + 10);
    if (mhz) *mhz = r[15];
    return true;
}

static bool upload(uint32_t addr, const uint8_t *img, size_t n, char *err, size_t errlen) {
    uint8_t block[512], c[16], args[9];
    uint32_t moved;
    for (size_t off = 0; off < n; off += 512) {
        size_t k = n - off < 512 ? n - off : 512;
        crc_cipher(img + off, block, k, 0xFFFFFFFF);  // the cipher restarts for every block
        uint16_t crc = crc16_update(0, block, k);
        be32(args, addr + off);
        args[4] = k >> 8;
        args[5] = k;
        args[6] = 0;
        args[7] = crc;  // CRC16 little-endian
        args[8] = crc >> 8;
        cdb(c, WRITE_MEMORY, args, 9);
        if (attempts(c, block, k, false, &moved, false, err, errlen) != host::OK) return false;
    }
    return true;
}

// Starts the program uploaded at addr. A jump whose answer was lost may have
// started it all the same: it is sent again only if no loader of ours answers
// from addr.
static bool jump_to_loader(uint32_t addr, char *err, size_t errlen) {
    uint8_t args[6], c[16], r[16];
    be32(args, addr);
    args[4] = LOADER_ARG >> 8;
    args[5] = LOADER_ARG;
    cdb(c, JUMP, args, 6);
    for (int attempt = 1; attempt <= 3; attempt++) {
        uint32_t moved;
        if (host::execute(c, 16, r, 16, true, &moved, err, errlen) == host::OK && ((r[0] << 8) | r[1]) == JUMP) {
            vTaskDelay(pdMS_TO_TICKS(100));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250 * attempt));
        char v[8];
        uint32_t at;
        if (identify(v, &at, nullptr) && at == addr) return true;
        if (cur) cur->retries++;
    }
    return false;
}

static bool start_own(bool running, uint32_t running_addr, Log log, char *err, size_t errlen) {
    bool use_a = !running || running_addr == MFLR_ADDR_B;
    uint32_t addr = use_a ? MFLR_ADDR_A : MFLR_ADDR_B;
    log("Uploading the M-Flasher loader to 0x%08lX", (unsigned long)addr);
    if (!upload(addr, use_a ? MFLR_A : MFLR_B, use_a ? sizeof MFLR_A : sizeof MFLR_B, err, errlen)) return false;
    if (!jump_to_loader(addr, err, errlen)) return false;
    char v[8];
    uint32_t at;
    if (!identify(v, &at, nullptr)) {
        snprintf(err, errlen, "the loader did not start");
        return false;
    }
    running_at = at;
    return true;
}

// Whether the loader found the flash when it started: without one it refuses
// every read as out of range.
static bool loader_has_flash() {
    uint8_t r[16], c[16], args[6], probe[32];
    char e[64];
    uint32_t moved;
    call16(LAST_ERROR, nullptr, 0, r, false, e, sizeof e);
    be32(args, 0);
    args[4] = 0;
    args[5] = 32;
    cdb(c, READ_FLASH, args, 6);
    host::execute(c, 16, probe, 32, true, &moved, e, sizeof e);
    if (call16(LAST_ERROR, nullptr, 0, r, false, e, sizeof e) == host::OK && r[2] == OUT_OF_RANGE) return false;
    return true;
}

static const char *bundled_version() {
    static char v[4];
    for (size_t i = 0; i + 7 <= sizeof MFLR_A; i++) {
        if (memcmp(MFLR_A + i, "MFLR", 4) == 0) {
            memcpy(v, MFLR_A + i + 4, 3);
            v[3] = 0;
            return v;
        }
    }
    return "?";
}

bool start(Info *info, Log log, char *err, size_t errlen) {
    memset(info, 0, sizeof *info);
    cur = info;

    // INQUIRY: must be the ROM's download mode
    uint8_t inq[36] = {0}, c6[6] = {0x12, 0, 0, 0, 36, 0};
    uint32_t got = 0;
    host::Result r = host::FAILED;
    for (int i = 1; i <= 3; i++) {
        r = host::execute(c6, 6, inq, 36, true, &got, err, errlen);
        if (r == host::OK && got >= 32) break;
        if (r == host::OK) {
            snprintf(err, errlen, "INQUIRY: %lu of 36 bytes", (unsigned long)got);
            r = host::FAILED;
        }
        vTaskDelay(pdMS_TO_TICKS(250 * i));
    }
    if (r != host::OK) return false;
    memcpy(info->vendor, inq + 8, 8);
    memcpy(info->product, inq + 16, 16);
    for (int i = 7; i >= 0 && info->vendor[i] == ' '; i--) info->vendor[i] = 0;
    for (int i = 15; i >= 0 && info->product[i] == ' '; i--) info->product[i] = 0;
    log("INQUIRY: %s %s", info->vendor, info->product);
    if (strcmp(info->vendor, "WL82") != 0 || strcmp(info->product, "UBOOT1.00") != 0) {
        snprintf(err, errlen, "\"%s %s\" is not WL82 UBOOT1.00", info->vendor, info->product);
        return false;
    }

    // What runs already? Ours, another tool's (refuse), or just the ROM.
    char ver[8];
    uint32_t at = 0;
    bool ours = identify(ver, &at, nullptr);
    if (!ours) {
        uint8_t resp[16];
        host::Result p = call16(ONLINE_DEVICE, nullptr, 0, resp, true, err, errlen);
        if (p == host::OK) {
            snprintf(err, errlen, "another flash loader runs: switch the unit off, then try again");
            return false;
        }
        if (p == host::FAILED) return false;
    }
    const char *bundled = bundled_version();
    if (ours && strcmp(ver, bundled) == 0) {
        log("M-Flasher loader %s already running", ver);
        running_at = at;
    } else if (!start_own(ours, at, log, err, errlen)) {
        return false;
    }
    if (!loader_has_flash()) {
        log("The loader found no flash: starting it again");
        if (!start_own(true, running_at, log, err, errlen)) return false;
        if (!loader_has_flash()) {
            snprintf(err, errlen, "the flash chip did not identify itself; nothing was written");
            return false;
        }
    }

    identify(info->loader, &info->loader_addr, &info->mhz);
    uint8_t resp[16], key_args[4] = {0x00, 0xAC, 0x69, 0x00};
    if (call16(READ_KEY, key_args, 4, resp, false, err, errlen) != host::OK) return false;
    uint8_t enc[2] = {resp[7], resp[6]}, plain[2];
    crc_cipher(enc, plain, 2, 0xFFFFFFFF);
    info->chip_key = plain[0] | plain[1] << 8;
    if (call16(ONLINE_DEVICE, nullptr, 0, resp, false, err, errlen) != host::OK) return false;
    info->flash_id = resp[4] | resp[5] << 8 | resp[6] << 16 | (uint32_t)resp[7] << 24;
    info->io = 512;
    if (call16(USB_BUFFER_SIZE, nullptr, 0, resp, false, err, errlen) == host::OK) {
        uint32_t io = rd_be32(resp + 2);
        info->io = io < 256 ? 256 : io > 4096 ? 4096 : io;
    }
    if (call16(UNIQUE_ID, nullptr, 0, resp, false, err, errlen) == host::OK) {
        memcpy(info->unique_id, resp + 2, 8);
        bool all00 = true, allFF = true;
        for (int i = 0; i < 8; i++) {
            all00 &= resp[2 + i] == 0;
            allFF &= resp[2 + i] == 0xFF;
        }
        info->has_unique_id = !all00 && !allFF;
    }
    err[0] = 0;
    return true;
}

bool read(uint32_t addr, uint8_t *buf, uint32_t n, char *err, size_t errlen) {
    uint32_t done = 0;
    while (done < n) {
        uint32_t k = n - done < READ_IO ? n - done : READ_IO;
        uint8_t args[6], c[16];
        be32(args, addr + done);
        args[4] = k >> 8;
        args[5] = k;
        cdb(c, READ_FLASH, args, 6);
        bool ok = false;
        for (int attempt = 1; attempt <= RETRIES && !ok; attempt++) {
            uint32_t moved = 0;
            host::Result r = host::execute(c, 16, buf + done, k, true, &moved, err, errlen);
            if (r == host::OK && moved == k) {
                ok = true;
            } else {
                if (r == host::OK) snprintf(err, errlen, "reading 0x%05lX: %lu of %lu bytes", (unsigned long)(addr + done),
                                            (unsigned long)moved, (unsigned long)k);
                if (!host::connected()) return false;
                if (attempt < RETRIES) {
                    if (cur) cur->retries++;
                    vTaskDelay(pdMS_TO_TICKS(250 * attempt));
                }
            }
        }
        if (!ok) return false;
        done += k;
    }
    return true;
}

bool erase(uint32_t addr, uint32_t n, char *err, size_t errlen) {
    uint32_t a = addr, end = addr + n;
    while (a < end) {
        bool block = a % 0x10000 == 0 && end - a >= 0x10000;
        uint8_t args[4], resp[16];
        be32(args, a);
        // a failed or refused erase is sent again (attempts): erasing twice is harmless
        if (call16(block ? ERASE_BLOCK : ERASE_SECTOR, args, 4, resp, false, err, errlen) != host::OK) {
            char e2[96];
            snprintf(e2, sizeof e2, "erasing 0x%05lX: %s", (unsigned long)a, err);
            snprintf(err, errlen, "%s", e2);
            return false;
        }
        a += block ? 0x10000 : 0x1000;
    }
    return true;
}

bool program(uint32_t addr, const uint8_t *data, uint32_t n, char *err, size_t errlen) {
    uint32_t io = cur && cur->io ? cur->io : 512;
    for (uint32_t off = 0; off < n; off += io) {
        uint32_t k = n - off < io ? n - off : io;
        bool blank = true;
        for (uint32_t i = 0; i < k && blank; i++) blank = data[off + i] == 0xFF;
        if (blank) continue;
        uint16_t crc = crc16_update(0, data + off, k);
        uint8_t args[9], c[16];
        be32(args, addr + off);
        args[4] = k >> 8;
        args[5] = k;
        args[6] = 0;
        args[7] = crc;  // CRC16 of the plain data, little-endian: the loader checks it
        args[8] = crc >> 8;
        cdb(c, WRITE_FLASH, args, 9);
        uint32_t moved;
        // programming the same data again is harmless (as M-Flasher's retry)
        if (attempts(c, (uint8_t *)data + off, k, false, &moved, false, err, errlen) != host::OK) {
            char e2[96];
            snprintf(e2, sizeof e2, "writing 0x%05lX: %s", (unsigned long)(addr + off), err);
            snprintf(err, errlen, "%s", e2);
            return false;
        }
    }
    return true;
}

bool crc32(uint32_t addr, uint32_t n, uint32_t *crc, char *err, size_t errlen) {
    uint8_t args[8], resp[16];
    be32(args, addr);
    be32(args + 4, n);
    if (call16(CRC32, args, 8, resp, false, err, errlen) != host::OK) return false;
    *crc = rd_be32(resp + 2);
    return true;
}

bool ping() {
    char v[8], e[64];
    uint32_t at;
    (void)e;
    return identify(v, &at, nullptr);
}

bool restart(char *err, size_t errlen) {
    // into the slot the loader does not use; the jump's answer is ignored
    uint32_t addr = running_at == MFLR_ADDR_A ? MFLR_ADDR_B : MFLR_ADDR_A;
    if (!upload(addr, RESTART_PROG, sizeof RESTART_PROG, err, errlen)) return false;
    uint8_t args[6], c[16], r[16];
    uint32_t moved;
    be32(args, addr);
    args[4] = LOADER_ARG >> 8;
    args[5] = LOADER_ARG;
    cdb(c, JUMP, args, 6);
    host::execute(c, 16, r, 16, true, &moved, err, errlen);
    return true;
}

}  // namespace jl
