// What both modes do with a unit, without any screen: download mode (key and
// calibration), the USB host and the loader, which unit it is, the backup.
// Everything is logged with ulog(); progress comes back through callbacks.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "jl.h"
#include "usbkey.h"

namespace engine {

static constexpr uint32_t FLASH_SIZE = 0x100000;
typedef void (*Progress)(uint32_t done, uint32_t total);

// --- download mode
enum EnterResult { ENTER_OK, ENTER_ALREADY_ON, ENTER_TIMEOUT, ENTER_CANCELLED, ENTER_FAILED };
struct EnterHooks {
    bool (*poll)(const usbkey::KeyStats *);  // while sending the key: true cancels
    void (*answered)(bool ack);               // the ROM answered: calibration starts
    bool (*cancel)();                         // during calibration: true cancels
};
EnterResult enter_download_mode(const EnterHooks &h, uint32_t timeout_ms, usbkey::KeyStats *ks);

// --- the USB host and the loader
enum ConnectResult { CONN_OK, CONN_HOST_ERROR, CONN_NO_DEVICE, CONN_NOT_ROM, CONN_LOADER_FAILED };
ConnectResult connect(jl::Info *info, char *err, size_t errlen);

// --- which unit: decided before anything can be written
enum Model { MODEL_UNKNOWN, MODEL_FM1, MODEL_SMK37 };
struct Unit {
    Model model;
    const char *name, *why;
};
extern Unit unit;  // set by connect()

// --- a full backup to the SD card, checked against the chip's CRC32 and read
// back from the card. path gets its path; smk: SMK-37 calibration marks seen.
bool backup(const jl::Info &info, char *path, size_t pathlen, bool *smk, Progress progress);

// Keeps the session busy; false when the unit is gone.
bool keepalive();

}  // namespace engine
