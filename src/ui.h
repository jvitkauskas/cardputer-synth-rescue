// Screen and keys: the log (to /fm1/log.txt and the advanced mode's console)
// and the guided mode's pages (a picture, a few large words, the key to press).
#pragma once
#include <M5Cardputer.h>

// Not a global reference: M5Cardputer lives in another file and may not be
// constructed yet when this file's globals are initialized.
#define lcd (M5Cardputer.Display)

// One line in /fm1/log.txt and the console's buffer (drawn only while the
// console is on screen).
void ulog(const char *fmt, ...);

namespace ui {

// colours of the guided pages
static constexpr uint16_t CYAN = 0x065F, GREEN = 0x2F2D, ORANGE = 0xFCE3, RED = 0xF9C6, YELLOW = 0xFEA1, GREY = 0x7BEF;

// --- keys
bool g0_pressed();
// '\n' ENTER or G0, 27 ESC (` or DEL), a lower-case letter or digit, or 0.
int read_key();
void wait_released();
bool any_key_cancel();

// --- the advanced mode's console: a title, the last log lines, a status line
void console(const char *title, uint16_t color);
void status(const char *fmt, ...);
void clear_log();
void show_console();  // the log as it is, for "details" from the guided mode

// --- guided pages: the picture on the left, the words on the right, and at
// the bottom the key to press (key nullptr: none), what it does, and a small
// grey label on the right (another key: "esc", "A: advanced").
void page(const char *art, uint16_t accent, const char *title, const char *body, const char *key = nullptr,
          const char *hint = nullptr, const char *right = "esc");
// A line in colour under the page's words (e.g. "Card found").
void note(uint16_t color, const char *text);
// A bar under the page's words (done of total).
void progress(uint32_t done, uint32_t total);
// The page's bottom line, replaced (e.g. a live state).
void footer(uint16_t color, const char *text);
// Three dots that move: call it often while waiting.
void spinner();

}  // namespace ui
