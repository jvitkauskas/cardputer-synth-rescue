#include "modes.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace modes {

static constexpr uint32_t MAGIC = 0x46314D44;  // "F1MD"
RTC_NOINIT_ATTR static uint32_t rtc_magic, rtc_mode;

void restart_into(Mode m) {
    rtc_magic = MAGIC;
    rtc_mode = m;
    ESP.restart();
    for (;;) {
    }
}

Mode at_boot() {
    Mode m = GUIDED;
    if (esp_reset_reason() == ESP_RST_SW && rtc_magic == MAGIC &&
        (rtc_mode == GUIDED || rtc_mode == ADVANCED || rtc_mode == GUIDED_SWITCH_ON))
        m = (Mode)rtc_mode;
    rtc_magic = 0;
    return m;
}

}  // namespace modes
