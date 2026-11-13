// The guided mode: the whole rescue as a few clear steps, one a screen. Each
// step that needs the user waits for its own key (1, 2, 3, Y, ...), so a key
// pressed twice never skips a step; everything else goes on by itself.
//
//   home     ENTER        start (A: the advanced mode)
//   1        switch it off
//   2        connect the cable
//   3        a backup card (optional; shows whether one is in)
//            switch it on: the Cardputer waits for it, connects, finds out
//            which unit it is
//            backup (with a card) or, without one, C: go on without
//            checking
//   Y        repair (confirm)
//            repairing, verified, the unit restarted
//   ENTER    done
//
// A problem shows what to do and its own "try again" key; V shows the log.
#include "guided.h"

#include "advanced.h"
#include "engine.h"
#include "modes.h"
#include "sd.h"
#include "ui.h"
#include "writer.h"

using engine::unit;

namespace guided {

static constexpr uint32_t KEY_TIMEOUT_MS = 90000;

struct Page {
    const char *art;
    uint16_t accent;
    const char *title, *body, *key, *hint;
    const char *right;  // the footer's small grey label
};

static void show(const Page &p) { ui::page(p.art, p.accent, p.title, p.body, p.key, p.hint, p.right); }

// Shows p and waits for one of `keys` (27 = ESC). V shows the log (on pages
// with `details`), any key back. With `session` the loader is kept busy, and
// a lost unit returns 0. tick runs while waiting (a live line, a spinner).
static int ask(const Page &p, const char *keys, bool details = false, bool session = false,
               void (*tick)() = nullptr) {
    show(p);
    ui::wait_released();
    uint32_t last_ping = millis();
    for (;;) {
        int k = ui::read_key();
        if (k && strchr(keys, k)) {
            ui::wait_released();
            return k;
        }
        if (k == 'v' && details) {
            ui::show_console();
            ui::wait_released();
            while (!ui::read_key()) delay(20);
            show(p);
            ui::wait_released();
        }
        if (tick) tick();
        delay(20);
        if (session && millis() - last_ping > 1000) {
            last_ping = millis();
            if (!engine::keepalive()) return 0;
        }
    }
}

[[noreturn]] static void finish() { modes::restart_into(modes::GUIDED); }

// The unit back to its firmware, the Cardputer back to the start.
[[noreturn]] static void leave(bool restart_unit) {
    char err[96];
    if (restart_unit) jl::restart(err, sizeof err);
    finish();
}

// ---------------------------------------------------------------------------
// Live parts of the pages

static void sd_tick() {
    static uint32_t last;
    static int shown = -1;
    if (millis() - last < 1500 && shown >= 0) return;
    last = millis();
    int now = sd_ready() ? 1 : 0;
    if (now != shown) {
        shown = now;
        ui::note(now ? ui::GREEN : ui::GREY, now ? "Card found" : "No card");
    }
}

static bool key_poll(const usbkey::KeyStats *) {
    ui::spinner();
    // only ESC cancels: a key pressed again for the step before must not
    M5Cardputer.update();
    auto &kb = M5Cardputer.Keyboard;
    if (kb.isChange() && kb.isPressed()) {
        auto st = kb.keysState();
        if (st.del || (!st.word.empty() && st.word[0] == '`')) return true;
    }
    return false;
}

static void answered(bool) { ui::page("connecting", ui::CYAN, "Connecting", "Keep it switched on and plugged in.", nullptr, nullptr); }
static bool no_cancel() { return false; }
static void bar(uint32_t done, uint32_t total) { ui::progress(done, total); }

// ---------------------------------------------------------------------------
// Steps

// Switch on, download mode, connection. False: back to the start.
static bool connect(jl::Info *info) {
    for (;;) {
        Page on = {"switch_on", ui::GREEN, "Switch it on", "Turn the synth on now. The Cardputer takes it from there.",
                   nullptr, "Waiting for it...  esc cancels", nullptr};
        show(on);
        usbkey::KeyStats ks;
        engine::EnterResult r = engine::enter_download_mode({key_poll, answered, no_cancel}, KEY_TIMEOUT_MS, &ks);
        if (r == engine::ENTER_CANCELLED) return false;
        if (r == engine::ENTER_ALREADY_ON) {
            Page p = {"switch_off", ui::ORANGE, "It's already on", "Switch the synth off first.", "4", "when it's off", "esc"};
            if (ask(p, "4\x1b") != '4') return false;
            continue;
        }
        if (r != engine::ENTER_OK) {
            Page p = {"error", ui::RED, "No answer",
                      ks.alive ? "Was it on before? Switch it off." : "Check the cable and that it's charged. Switch it off.",
                      "4", "to try again", "esc"};
            if (ask(p, "4\x1b", true) != '4') return false;
            continue;
        }
        char err[96];
        engine::ConnectResult c = engine::connect(info, err, sizeof err);
        if (c == engine::CONN_OK) return true;
        // the USB host is up: a new try starts with a restart of the Cardputer
        Page p = c == engine::CONN_NOT_ROM
                     ? Page{"error", ui::ORANGE, "It started normally", "Its own firmware is running. Switch it off.", "4",
                            "to try again", "esc"}
                     : Page{"error", ui::RED, "No connection", "Switch the synth off, check the cable.", "4", "to try again",
                            "esc"};
        if (ask(p, "4\x1b", true) != '4') finish();
        modes::restart_into(modes::GUIDED_SWITCH_ON);
    }
}

// The backup, or going on without one. False: stop (nothing written).
static bool backup_or_not(const jl::Info &info) {
    for (;;) {
        if (sd_ready()) {
            ui::page("backup", ui::CYAN, "Saving a backup", "Copying the synth to the SD card.", nullptr, nullptr);
            ui::progress(0, 1);
            bool smk = false;
            char path[48];
            if (engine::backup(info, path, sizeof path, &smk, bar)) {
                if (unit.model == engine::MODEL_FM1 && smk) {
                    ulog("SMK-37 calibration data on an FM-1: stop.");
                    Page p = {"error", ui::RED, "Unknown device", "It is not the unit it seemed. Nothing was changed.",
                              "ENTER", "to finish", nullptr};
                    ask(p, "\n", true, true);
                    return false;
                }
                return true;
            }
            Page p = {"error", ui::RED, "Backup failed",
                      "Nothing changed. Try another card, or take it out to skip it.", "5", "to try again",
                      "esc"};
            int k = ask(p, "5\x1b", true, true);
            if (k != '5') return false;
            continue;
        }
        Page p = {"warning", ui::YELLOW, "No backup card",
                  "Without one, what is on the synth now is lost for good.", "C", "go on", "esc"};
        int k = ask(p, "c\x1b", false, true, sd_tick);
        if (k != 'c') return false;
        if (sd_ready()) continue;  // a card went in meanwhile: back up after all
        ulog("No SD card: rescue without a backup.");
        return true;
    }
}

void run(bool switch_on) {
    for (;;) {
        if (!switch_on) {
            Page home = {"welcome", ui::CYAN, "Synth rescue", "An FM-1 or SMK-37 that won't start? Let's fix it.",
                         "ENTER", "start", "A: advanced"};
            int k = ask(home, "\na");
            if (k == 'a') {
                advanced::run();
                continue;
            }
            Page off = {"switch_off", ui::CYAN, "Switch it off", "Turn the synth's power switch off.", "1",
                        "when it's off", "esc"};
            if (ask(off, "1\x1b") != '1') continue;
            Page cable = {"connect", ui::CYAN, "Connect it", "USB-C cable from the synth to this Cardputer.", "2",
                          "when connected", "esc"};
            if (ask(cable, "2\x1b") != '2') continue;
            Page card = {"sd_card", ui::CYAN, "Backup card", "A microSD card keeps a copy of the synth. Optional.", "3",
                         "to go on", "esc"};
            if (ask(card, "3\x1b", false, false, sd_tick) != '3') continue;
        }
        switch_on = false;

        jl::Info info;
        if (!connect(&info)) continue;

        bool fm1 = unit.model == engine::MODEL_FM1, smk37 = unit.model == engine::MODEL_SMK37;
        if (!fm1 && !smk37) {
            Page p = {"error", ui::RED, "Unknown device", "Not an FM-1 or SMK-37. Nothing was changed.", "ENTER",
                      "to finish", nullptr};
            ask(p, "\n", true, true);
            leave(true);
        }
        const char *art = fm1 ? "fm1" : "smk37";
        char found[32];
        snprintf(found, sizeof found, "Found: %s", unit.name);
        ui::page(art, ui::CYAN, found, "Connected.", nullptr, nullptr);
        delay(1500);

        const writer::Image &img = writer::image(smk37 ? writer::SMK37 : writer::FM1);
        char err[128];
        if (!writer::image_ok(img, err, sizeof err)) {
            ulog("%s", err);
            Page p = {"error", ui::RED, "Can't repair", "This app's repair image is damaged: install the app again.",
                      "ENTER", "to finish", nullptr};
            ask(p, "\n", true, true);
            leave(true);
        }

        if (!backup_or_not(info)) leave(true);

        // what to write
        writer::Plan plan;
        for (;;) {
            ui::page("repair", ui::ORANGE, "Checking", "Comparing the synth with the repair.", nullptr, nullptr);
            ui::progress(0, 1);
            if (writer::plan(img, &plan, bar, err, sizeof err)) break;
            ulog("Reading the unit failed: %s", err);
            Page p = {"error", ui::RED, "Can't read it", "Nothing was changed. Keep it on and plugged in.", "6",
                      "to try again", "esc"};
            if (ask(p, "6\x1b", true, true) != '6') leave(true);
        }
        char done_text[96];
        if (!plan.to_write && !plan.to_erase) {
            jl::restart(err, sizeof err);
            snprintf(done_text, sizeof done_text, "The %s already had the repair. It restarts now.", unit.name);
            Page p = {"success", ui::GREEN, "All good", done_text, "ENTER", "to finish", nullptr};
            ask(p, "\n");
            finish();
        }

        char body[96];
        snprintf(body, sizeof body, "The %s gets a fresh bootloader; its old firmware and settings are erased.", unit.name);
        Page confirm = {art, ui::ORANGE, "Ready to repair", body, "Y", "repair now", "esc"};
        if (ask(confirm, "y\x1b", false, true) != 'y') leave(true);

        for (;;) {
            ui::page("repair", ui::ORANGE, "Repairing", "Don't switch it off or unplug it.", nullptr, nullptr);
            ui::progress(0, 1);
            uint32_t t0 = millis();
            if (writer::apply(img, plan, ulog, bar, err, sizeof err)) {
                ulog("Rescue installed in %lu s.", (unsigned long)((millis() - t0) / 1000));
                break;
            }
            ulog("Write failed: %s", err);
            Page p = {"error", ui::RED, "Repair failed", "Don't switch it off. The Cardputer can try again.", "7",
                      "to try again", "esc"};
            // ESC restarts the Cardputer: the unit can always be reached again
            if (ask(p, "7\x1b", true) == 27) finish();
            // read it again: what the failed try left
            while (!writer::plan(img, &plan, bar, err, sizeof err)) {
                ulog("Reading the unit failed: %s", err);
                Page q = {"error", ui::RED, "Can't read it", "Don't switch it off. Keep it plugged in.", "7",
                          "to try again", "esc"};
                if (ask(q, "7\x1b", true) == 27) finish();
            }
        }
        jl::restart(err, sizeof err);
        snprintf(done_text, sizeof done_text, "The %s waits in MIDI mode: install a firmware from your phone or computer.",
                 unit.name);
        Page ok = {"success", ui::GREEN, "Repaired!", done_text, "ENTER", "to finish", nullptr};
        ask(ok, "\n");
        finish();
    }
}

}  // namespace guided
