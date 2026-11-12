// JieLi UBOOT1.00 (mask ROM download mode) and M-Flasher's flash loader,
// ported from M-Flasher's src/jl.rs (MIT).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace jl {

uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n);  // CRC16/XMODEM
uint32_t crc32_update(uint32_t crc, const uint8_t *d, size_t n);  // IEEE; start and end with ~0 applied inside
void crc_cipher(const uint8_t *in, uint8_t *out, size_t n, uint32_t key);

struct Info {
    char vendor[9], product[17];
    char loader[8];         // version of the running M-Flasher loader
    uint32_t loader_addr;
    uint8_t mhz;
    uint16_t chip_key;
    uint32_t flash_id;      // JEDEC, e.g. 0x856014
    uint32_t io;            // write chunk size from FC14
    uint8_t unique_id[8];
    bool has_unique_id;
    uint32_t retries;
};

typedef void (*Log)(const char *fmt, ...);

// INQUIRY, then starts (or reuses) M-Flasher's loader and reads the identity.
bool start(Info *info, Log log, char *err, size_t errlen);
// Reads n bytes at addr (chunks of up to 16 KiB).
bool read(uint32_t addr, uint8_t *buf, uint32_t n, char *err, size_t errlen);
// CRC32 of n bytes at addr, computed on the chip.
bool crc32(uint32_t addr, uint32_t n, uint32_t *crc, char *err, size_t errlen);
// Erases [addr, addr + n): 64 KiB blocks where aligned, else 4 KiB sectors.
bool erase(uint32_t addr, uint32_t n, char *err, size_t errlen);
// Programs erased flash in chunks of the loader's buffer size; all-0xFF
// chunks are skipped (erased flash already reads so).
bool program(uint32_t addr, const uint8_t *data, uint32_t n, char *err, size_t errlen);
// A harmless command: keeps the session busy while waiting for the user.
bool ping();
// Leaves download mode: the unit boots its application.
bool restart(char *err, size_t errlen);

}  // namespace jl
