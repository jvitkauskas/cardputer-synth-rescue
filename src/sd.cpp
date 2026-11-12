#include "sd.h"

#include <SPI.h>

bool sd_ok;

bool sd_mount() {
    static bool spi_started;
    if (!spi_started) {
        SPI.begin(40, 39, 14, 12);  // SCK, MISO, MOSI, CS (Cardputer ADV)
        spi_started = true;
    }
    SD.end();
    sd_ok = SD.begin(12, SPI, 20000000);
    if (sd_ok) SD.mkdir("/fm1");
    return sd_ok;
}

bool sd_ready() {
    if (sd_ok && SD.exists("/fm1")) return true;
    return sd_mount();
}
