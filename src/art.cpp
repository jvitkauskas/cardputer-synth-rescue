#include "art.h"

#include <string.h>

#include "ui.h"

namespace art {

// A stand-in when a picture is missing: the synth, or a big symbol.
static void placeholder(const char *name, int x, int y, int w, int h, uint16_t c) {
    int cx = x + w / 2, cy = y + h / 2, r = w / 3;
    if (!strcmp(name, "success")) {
        lcd.fillCircle(cx, cy, r, c);
        lcd.drawWideLine(cx - r / 2, cy, cx - r / 8, cy + r / 2 - 4, 5, TFT_BLACK);
        lcd.drawWideLine(cx - r / 8, cy + r / 2 - 4, cx + r / 2 + 2, cy - r / 2 + 2, 5, TFT_BLACK);
    } else if (!strcmp(name, "error")) {
        lcd.fillCircle(cx, cy, r, c);
        lcd.drawWideLine(cx - r / 2, cy - r / 2, cx + r / 2, cy + r / 2, 5, TFT_BLACK);
        lcd.drawWideLine(cx - r / 2, cy + r / 2, cx + r / 2, cy - r / 2, 5, TFT_BLACK);
    } else if (!strcmp(name, "warning")) {
        lcd.fillTriangle(cx, cy - r, cx - r - 4, cy + r - 4, cx + r + 4, cy + r - 4, c);
        lcd.fillRect(cx - 3, cy - r / 2, 7, r - 2, TFT_BLACK);
        lcd.fillCircle(cx, cy + r / 2 + 4, 4, TFT_BLACK);
    } else {
        // the synth: a box, a screen, two knobs, keys
        int bx = x + 8, by = y + 26, bw = w - 16, bh = h - 46;
        lcd.drawRoundRect(bx, by, bw, bh, 6, TFT_WHITE);
        lcd.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 5, TFT_WHITE);
        lcd.fillRect(bx + 8, by + 8, bw / 3, bh / 3, c);
        lcd.fillCircle(bx + bw - 30, by + 14, 6, TFT_LIGHTGREY);
        lcd.fillCircle(bx + bw - 14, by + 14, 6, TFT_LIGHTGREY);
        for (int k = 0; k < 7; k++) lcd.fillRect(bx + 8 + k * (bw - 16) / 7, by + bh - 16, (bw - 16) / 7 - 2, 12, TFT_WHITE);
    }
}

void draw(const char *name, int x, int y, int w, int h, uint16_t accent) {
    for (int i = 0; i < COUNT; i++) {
        if (!strcmp(TABLE[i].name, name)) {
            if (lcd.drawPng(TABLE[i].data, TABLE[i].len, x, y, w, h)) return;
            break;
        }
    }
    placeholder(name, x, y, w, h, accent);
}

}  // namespace art
