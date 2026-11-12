#include "engine.h"

#include <Arduino.h>
#include <string.h>

#include "sd.h"
#include "uboot_host.h"
#include "ui.h"

namespace engine {

static constexpr uint32_t CHUNK = 16384;
static uint8_t chunk_buf[CHUNK];
Unit unit;

// ---------------------------------------------------------------------------
// Download mode

EnterResult enter_download_mode(const EnterHooks &h, uint32_t timeout_ms, usbkey::KeyStats *ks) {
    ulog("Sending the key; switch the unit ON.");
    usbkey::KeyResult kr = usbkey::send_key(ks, timeout_ms, h.poll);
    switch (kr) {
        case usbkey::DP_HIGH_AT_START:
            ulog("D+ is high: the unit is already on.");
            return ENTER_ALREADY_ON;
        case usbkey::TIMEOUT:
            ulog("No ACK in %lu s: %lu packets, %lu false ACK, line %s.", (unsigned long)(timeout_ms / 1000),
                 (unsigned long)ks->packets, (unsigned long)ks->false_acks, ks->alive ? "alive" : "dead");
            return ENTER_TIMEOUT;
        case usbkey::CANCELLED:
            ulog("Key cancelled.");
            return ENTER_CANCELLED;
        default:
            break;
    }
    ulog("%s after %lu packets (%lu false ACK)", kr == usbkey::ANSWER_ACK ? "ACK" : "D+ up (ACK missed)",
         (unsigned long)ks->packets, (unsigned long)ks->false_acks);
    if (h.answered) h.answered(kr == usbkey::ANSWER_ACK);
    usbkey::PulseResult pr = usbkey::calibrate(h.cancel);
    if (pr.rmt_error) {
        ulog("Could not start the pulse generator.");
        return ENTER_FAILED;
    }
    if (pr.cancelled) {
        ulog("Stopped during calibration.");
        return ENTER_CANCELLED;
    }
    ulog("%lld ms of pulses, D+ %s, now %d", (long long)pr.pulse_ms,
         pr.drop_ms >= 0 ? "dropped (cal done)" : "still high", usbkey::dp());
    return ENTER_OK;
}

// ---------------------------------------------------------------------------
// Which unit

// M-Flasher's evidence (boards.rs): the SMK-37's calibration record (trimdat
// at 0xC1224, m-bootloader's TRIMCOPY at 0xFF000) decides; else the flash ID
// (FM-1 0x856014 / 0xB36014, SMK-37 0xEB6014).
static Unit identify_unit(const jl::Info &info) {
    uint8_t a[32] = {0}, b[32] = {0};
    char err[96];
    bool trimdat = jl::read(0xC1220, a, sizeof a, err, sizeof err) && memcmp(a + 4, "trimdat\0", 8) == 0;
    bool trimcopy = jl::read(0xFF000, b, sizeof b, err, sizeof err) && memcmp(b, "TRIMCOPY", 8) == 0;
    if (info.chip_key != 0x980F) return {MODEL_UNKNOWN, "unknown unit", "chip key is not 980F"};
    if (trimdat || trimcopy) return {MODEL_SMK37, "SMK-37", "its calibration record"};
    if (info.flash_id == 0xEB6014) return {MODEL_SMK37, "SMK-37", "flash ID"};
    if (info.flash_id == 0x856014 || info.flash_id == 0xB36014) return {MODEL_FM1, "FM-1", "flash ID, no SMK-37 data"};
    return {MODEL_UNKNOWN, "unknown unit", "flash ID not known"};
}

// ---------------------------------------------------------------------------
// USB host and loader

ConnectResult connect(jl::Info *info, char *err, size_t errlen) {
    usbkey::bus_float();
    if (!host::start()) {
        snprintf(err, errlen, "could not start the USB host");
        ulog("%s", err);
        return CONN_HOST_ERROR;
    }
    uint16_t vid = 0, pid = 0;
    if (!host::wait_device(12000, &vid, &pid, ulog, err, errlen)) {
        ulog("%s", err);
        if (vid) {
            ulog("Device %04X:%04X: not the download mode", vid, pid);
            return CONN_NOT_ROM;
        }
        return CONN_NO_DEVICE;
    }
    ulog("USB device %04X:%04X", vid, pid);
    if (vid != 0x4C4A || pid != 0x8057) {
        snprintf(err, errlen, "%04X:%04X is not the download mode", vid, pid);
        ulog("%s", err);
        return CONN_NOT_ROM;
    }
    if (!jl::start(info, ulog, err, errlen)) {
        ulog("%s", err);
        return CONN_LOADER_FAILED;
    }
    ulog("Loader M-Flasher %s at %08lX, %u MHz", info->loader, (unsigned long)info->loader_addr, info->mhz);
    ulog("Chip key %04X, flash ID %06lX", info->chip_key, (unsigned long)info->flash_id);
    unit = identify_unit(*info);
    ulog("Unit: %s (%s)", unit.name, unit.why);
    if (info->retries) ulog("%lu transfers retried", (unsigned long)info->retries);
    return CONN_OK;
}

bool keepalive() { return jl::ping() || host::connected(); }

// ---------------------------------------------------------------------------
// Backup

static const char *model_hint(uint32_t jedec, bool trimdat, bool trimcopy) {
    if (trimdat || trimcopy) return "SMK-37 (calibration record)";
    if (jedec == 0x856014 || jedec == 0xB36014) return "FM-1 (by flash ID)";
    if (jedec == 0xEB6014) return "SMK-37 (by flash ID)";
    return "unknown";
}

// Reads the whole flash into path; returns its CRC32 (and the model marks).
static bool read_to_file(const char *path, uint32_t *crc, bool *trimdat, bool *trimcopy, Progress progress,
                         uint32_t pass) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        ulog("Cannot create %s", path);
        return false;
    }
    char err[96];
    *crc = 0;
    uint32_t t0 = millis();
    for (uint32_t a = 0; a < FLASH_SIZE; a += CHUNK) {
        if (!jl::read(a, chunk_buf, CHUNK, err, sizeof err)) {
            f.close();
            ulog("Read failed: %s", err);
            return false;
        }
        if (f.write(chunk_buf, CHUNK) != CHUNK) {
            f.close();
            ulog("SD write failed at 0x%05lX", (unsigned long)a);
            return false;
        }
        *crc = jl::crc32_update(*crc, chunk_buf, CHUNK);
        if (a <= 0xC1224 && 0xC1224 + 8 <= a + CHUNK) *trimdat = memcmp(chunk_buf + (0xC1224 - a), "trimdat\0", 8) == 0;
        if (a <= 0xFF000 && 0xFF000 + 8 <= a + CHUNK) *trimcopy = memcmp(chunk_buf + (0xFF000 - a), "TRIMCOPY", 8) == 0;
        // reading is most of the work; a second pass (rare) starts the bar again
        if (progress) progress(a + CHUNK, FLASH_SIZE + FLASH_SIZE / 8);
    }
    f.close();
    ulog("Read 1 MiB in %lu ms (pass %lu)", (unsigned long)(millis() - t0), (unsigned long)pass);
    return true;
}

static bool file_crc(const char *path, uint32_t *crc) {
    File f = SD.open(path, FILE_READ);
    if (!f || f.size() != FLASH_SIZE) return false;
    *crc = 0;
    for (uint32_t a = 0; a < FLASH_SIZE; a += CHUNK) {
        if (f.read(chunk_buf, CHUNK) != (int)CHUNK) {
            f.close();
            return false;
        }
        *crc = jl::crc32_update(*crc, chunk_buf, CHUNK);
    }
    f.close();
    return true;
}

bool backup(const jl::Info &info, char *out_path, size_t pathlen, bool *smk, Progress progress) {
    if (!sd_ready()) {
        ulog("No SD card: insert one (FAT32), try again.");
        return false;
    }
    char base[40], path[48], err[96];
    for (int n = 1; n < 1000; n++) {
        snprintf(base, sizeof base, "/fm1/backup-%03d", n);
        snprintf(path, sizeof path, "%s.bin", base);
        if (!SD.exists(path)) break;
    }
    ulog("Backing up to %s", path);
    uint32_t chip_crc;
    if (!jl::crc32(0, FLASH_SIZE, &chip_crc, err, sizeof err)) {
        ulog("Chip CRC failed: %s", err);
        return false;
    }
    bool trimdat = false, trimcopy = false;
    uint32_t crc = 0;
    bool ok = false;
    for (uint32_t pass = 1; pass <= 2 && !ok; pass++) {
        if (!read_to_file(path, &crc, &trimdat, &trimcopy, progress, pass)) return false;
        ok = crc == chip_crc;
        if (!ok) ulog("Pass %lu: CRC %08lX, chip says %08lX", (unsigned long)pass, (unsigned long)crc, (unsigned long)chip_crc);
    }
    if (!ok) {
        ulog("The reads do not match the chip: backup NOT valid.");
        return false;
    }
    uint32_t back;
    if (!file_crc(path, &back) || back != chip_crc) {
        ulog("Read back from SD does not match: NOT valid.");
        return false;
    }
    if (progress) progress(1, 1);
    if (out_path) snprintf(out_path, pathlen, "%s", path);
    if (smk) *smk = trimdat || trimcopy;
    snprintf(path, sizeof path, "%s.txt", base);
    File f = SD.open(path, FILE_WRITE);
    if (f) {
        f.printf("unit: %s\n", model_hint(info.flash_id, trimdat, trimcopy));
        f.printf("chip key: %04X\nflash id: %06lX\n", info.chip_key, (unsigned long)info.flash_id);
        if (info.has_unique_id) {
            f.print("unit id: ");
            for (int i = 0; i < 8; i++) f.printf("%02x", info.unique_id[i]);
            f.print("\n");
        }
        f.printf("loader: M-Flasher %s, CPU %u MHz\n", info.loader, info.mhz);
        f.printf("size: %lu\ncrc32: %08lx\nretries: %lu\n", (unsigned long)FLASH_SIZE, (unsigned long)chip_crc,
                 (unsigned long)info.retries);
        f.close();
    }
    ulog("OK: CRC32 %08lX matches the chip.", (unsigned long)chip_crc);
    ulog("Unit: %s", model_hint(info.flash_id, trimdat, trimcopy));
    return true;
}

}  // namespace engine
