// JieLi USB_KEY entry on the Cardputer's USB-C pins (GPIO20 = D+, GPIO19 = D-)
// and the ROM's calibration pulses. No UI here.
#pragma once
#include <stdint.h>

namespace usbkey {

struct KeyStats {
    uint32_t packets, false_acks;
    bool alive;  // the data line answered a probe: a powered unit is attached
};

enum KeyResult { ANSWER_ACK, ANSWER_DP, DP_HIGH_AT_START, TIMEOUT, CANCELLED };

struct PulseResult {
    int64_t pulse_ms;  // how long the pulses ran
    int64_t drop_ms;   // when D+ dropped (calibration done), or -1
    bool cancelled, rmt_error;
};

void init();
// Both lines GPIO, released, weak pull-down; the USB PHY detached.
void bus_init();
// Both lines released with no pulls, for the USB host to take over.
void bus_float();
bool dp();

// Sends the key until the ROM answers (ACK, then D+ pulled up). poll is called
// about every 100 ms with the counts; returning true cancels.
KeyResult send_key(KeyStats *s, uint32_t timeout_ms, bool (*poll)(const KeyStats *));
// 1 ms calibration pulses until D+ drops after >= 1 s, or 6 s. The pins are
// GPIO again (bus_init) when it returns.
PulseResult calibrate(bool (*cancel)());

// Nothing plugged in: both lines must follow the internal pulls (hi 3, lo 0).
void pin_check(int *hi, int *lo);
// Nothing plugged in: the pulse period measured on D+ over 3 s.
bool pulse_test(uint32_t *edges, double *period_us);

}  // namespace usbkey
