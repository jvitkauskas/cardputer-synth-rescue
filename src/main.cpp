// FM-1 / SMK-37 recovery for the M5Stack Cardputer ADV (ESP32-S3).
//
// Puts a JieLi WL82 (M-VAVE FM-1, SMK-37) into its ROM download mode through
// the Cardputer's own USB-C port (usbkey.cpp), becomes the USB host
// (uboot_host.cpp), starts M-Flasher's loader (jl.cpp) and backs the unit up
// to the SD card (engine.cpp) or installs a rescue image (writer.cpp).
//
// Two ways through it: the guided mode (guided.cpp, the default: a picture,
// a few words and one key a step) and the advanced mode (advanced.cpp: the
// log on screen, each step under control, self-tests, hand-off to a PC).

#include <M5Cardputer.h>

#include "advanced.h"
#include "guided.h"
#include "modes.h"
#include "sd.h"
#include "ui.h"
#include "usbkey.h"

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    // Landscape. Newer M5GFX already rotates the ADV panel (rotation_offset 1),
    // so rotation 0 is landscape there; older versions need 1 (as in M5's examples).
    lcd.setRotation(0);
    if (lcd.width() < lcd.height()) lcd.setRotation(1);
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);
    usbkey::init();  // take the USB pins away from the USB PHY right away
    if (sd_mount()) {
        File f = SD.open("/fm1/log.txt", FILE_APPEND);
        if (f) {
            f.printf("\n--- boot, synth rescue %s\n", APP_VERSION);
            f.close();
        }
    }
}

void loop() {
    modes::Mode m = modes::at_boot();
    if (m == modes::ADVANCED) advanced::run();
    guided::run(m == modes::GUIDED_SWITCH_ON);
}
