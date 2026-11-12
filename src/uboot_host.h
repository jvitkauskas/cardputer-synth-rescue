// USB host side of the JieLi ROM download mode: the ESP32-S3's USB host on the
// Cardputer's USB-C port, and the mass storage Bulk-Only Transport the ROM
// (WL82 UBOOT1.00, 4C4A:8057) and M-Flasher's loader speak.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace host {

enum Result { OK = 0, REFUSED = 1, FAILED = 2 };

// Installs the USB host on the internal PHY (takes GPIO19/20 from the app).
bool start();
typedef void (*Log)(const char *fmt, ...);

// Waits for a device to enumerate; fills vid/pid. Claims its bulk interface.
// Logs the root port's state as it changes, and ESP-IDF's own USB messages.
bool wait_device(uint32_t timeout_ms, uint16_t *vid, uint16_t *pid, Log log, char *err, size_t errlen);
bool connected();

// One Bulk-Only Transport command. in: data from the device into buf.
// moved: data bytes actually transferred. err: text for a non-OK result.
Result execute(const uint8_t *cdb, uint8_t cdb_len, uint8_t *buf, uint32_t len, bool in,
               uint32_t *moved, char *err, size_t errlen);

}  // namespace host
