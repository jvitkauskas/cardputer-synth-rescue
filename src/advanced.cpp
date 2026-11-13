// The advanced mode: the log on screen, every step under the user's control,
// the self-tests and the hand-off to a PC.
#include "advanced.h"

#include "engine.h"
#include "modes.h"
#include "sd.h"
#include "uboot_host.h"
#include "ui.h"
#include "writer.h"

using engine::unit;

namespace advanced {

static constexpr uint32_t KEY_TIMEOUT_MS = 60000;

static void idle_screen() {
    ui::clear_log();
    ui::console("Advanced", TFT_CYAN);
    ulog("FM-1/SMK-37 OFF, connect to the USB-C.");
    ulog("Run on battery: no PC/charger attached.");
    ulog("ENTER/G0  connect (backup, rescue)");
    ulog("H         key only, then move to a PC");
    ulog("P / T     pin check / pulse test");
    ulog("          (nothing plugged in)");
    ulog("ESC       back to the guided mode");
    ulog("Synth rescue %s", APP_VERSION);
    ui::status(sd_ok ? "SD card ready" : "No SD card: backups need one");
}

static uint32_t key_start_ms;

static bool key_poll(const usbkey::KeyStats *s) {
    static uint32_t last_draw;
    uint32_t now = millis();
    if (now - last_draw > 500) {
        last_draw = now;
        ui::status("%lu packets %lus line %s", (unsigned long)s->packets,
                   (unsigned long)((now - key_start_ms) / 1000), s->alive ? "alive" : "-");
        vTaskDelay(1);
    }
    return ui::any_key_cancel();
}

static void calibrating(bool) { ui::console("Calibrating", TFT_YELLOW); }
static bool g0_cancel() { return ui::g0_pressed(); }

// Key and calibration. True when the ROM answered and calibrated.
static bool enter_download_mode() {
    ui::clear_log();
    ui::console("Sending key", TFT_YELLOW);
    ulog("Switch the unit ON now.");
    ulog("Any key or G0 cancels.");
    key_start_ms = millis();
    usbkey::KeyStats ks;
    engine::EnterResult r = engine::enter_download_mode({key_poll, calibrating, g0_cancel}, KEY_TIMEOUT_MS, &ks);
    switch (r) {
        case engine::ENTER_OK:
            return true;
        case engine::ENTER_ALREADY_ON:
            ui::console("D+ is high", TFT_ORANGE);
            ulog("Switch the unit off and try again.");
            return false;
        case engine::ENTER_TIMEOUT:
            ui::console("No answer", TFT_RED);
            ulog(ks.alive ? "A unit was seen: was it OFF at the start?" : "Check the cable and the unit's battery.");
            return false;
        case engine::ENTER_CANCELLED:
            ui::console("Cancelled", TFT_ORANGE);
            ui::wait_released();
            return false;
        default:
            ui::console("Failed", TFT_RED);
            return false;
    }
}

static void run_handoff() {
    if (!enter_download_mode()) return;
    ui::console("MOVE CABLE TO PC", TFT_GREEN);
    ulog("Move the cable from the Cardputer to the");
    ulog("PC now, keeping the unit on.");
    ulog("Expect 4C4A:8057 WL82 UBOOT1.00");
    ui::status("");
}

// Waits for one of the keys in `keys` (27 = ESC), keeping the session busy.
static int choose(const char *keys) {
    ui::wait_released();
    uint32_t last_ping = millis();
    for (;;) {
        int k = ui::read_key();
        if (k && strchr(keys, k)) return k;
        delay(20);
        if (millis() - last_ping > 1000) {
            last_ping = millis();
            if (!engine::keepalive()) return 0;
        }
    }
}

static void read_progress(uint32_t done, uint32_t total) {
    ui::status("Reading %3lu%%", (unsigned long)(done * 100 / total));
}
static void compare_progress(uint32_t done, uint32_t total) {
    ui::status("Comparing %lu/%lu sectors", (unsigned long)done, (unsigned long)total);
}
static void write_progress(uint32_t done, uint32_t total) {
    ui::status("Writing %lu/%lu sectors - keep it on", (unsigned long)done, (unsigned long)total);
}

static bool backup(const jl::Info &info, char *path = nullptr, size_t n = 0, bool *smk = nullptr) {
    bool ok = engine::backup(info, path, n, smk, read_progress);
    ui::status(ok ? "Backup done" : "Backup FAILED");
    return ok;
}

static void rescue(const jl::Info &info) {
    char err[128], bak[48];
    ui::clear_log();
    ui::console("Install rescue", TFT_ORANGE);
    bool fm1 = unit.model == engine::MODEL_FM1, smk37 = unit.model == engine::MODEL_SMK37;
    if (info.chip_key != 0x980F || !(fm1 || smk37)) {
        ulog("Unknown unit (key %04X, flash %06lX):", info.chip_key, (unsigned long)info.flash_id);
        ulog("no rescue image for it.");
        return;
    }
    const writer::Image &img = writer::image(smk37 ? writer::SMK37 : writer::FM1);
    if (!writer::image_ok(img, err, sizeof err)) {
        ulog("%s", err);
        return;
    }
    ulog("%s: m-bootloader 2.3 only. Everything", img.name);
    ulog("else up to 0xFEFFF is ERASED (the old");
    ulog("firmware, presets, settings); it then");
    ulog("waits in MIDI mode for a firmware.");
    if (smk37) ulog("Kept: 0xC0000-0xC1FFF (calibration,");
    ulog(smk37 ? "Bluetooth), 0xFF000-. Backup first." : "Kept: 0xFF000-. A backup comes first.");
    ui::status("ENTER continue  ESC cancel");
    if (choose("\n\x1b") != '\n') {
        ulog("Cancelled: nothing written.");
        return;
    }
    // A backup first, if there is a card. Without one the unit is usually
    // past saving anyway: the rescue may go on, once that is understood.
    bool have_backup = sd_ready();
    if (have_backup) {
        bool smk_marks = false;
        if (!backup(info, bak, sizeof bak, &smk_marks)) {
            ulog("No valid backup: nothing written.");
            ulog("(Without a card the rescue can go on");
            ulog("with no backup.)");
            return;
        }
        if (fm1 && smk_marks) {
            ulog("SMK-37 calibration data found: this is");
            ulog("not an FM-1. Nothing written.");
            return;
        }
    } else {
        ui::clear_log();
        ui::console("No SD card", TFT_RED);
        ulog("No backup is possible. What the unit");
        ulog("holds now (firmware, settings, presets)");
        ulog("is lost for good if you go on.");
        ulog("Insert a card (FAT32) to keep a backup.");
        ui::status("C continue, no backup  ESC cancel");
        if (choose("c\x1b") != 'c') {
            ulog("Cancelled: nothing written.");
            return;
        }
        have_backup = sd_ready();  // a card went in meanwhile: back up after all
        if (have_backup && !backup(info, bak, sizeof bak)) {
            ulog("No valid backup: nothing written.");
            return;
        }
    }
    ui::status("Comparing the unit with the image...");
    writer::Plan plan;
    if (!writer::plan(img, &plan, compare_progress, err, sizeof err)) {
        ulog("Reading the unit failed: %s", err);
        return;
    }
    ui::clear_log();
    ui::console("Ready to write", TFT_ORANGE);
    if (have_backup) ulog("%s rescue. Backup: %s", img.name, bak + 5);
    else ulog("%s rescue. NO BACKUP.", img.name);
    if (!plan.to_write && !plan.to_erase) {
        ulog("The unit already holds the image.");
        return;
    }
    ulog("%lu sectors to write, %lu to erase,", (unsigned long)plan.to_write, (unsigned long)plan.to_erase);
    ulog("in %lu runs.", (unsigned long)plan.runs);
    ulog("Do not switch the unit off or unplug it");
    ulog("while writing.");
    ui::status("W write now  ESC cancel");
    if (choose("w\x1b") != 'w') {
        ulog("Cancelled: nothing written.");
        return;
    }
    ui::clear_log();
    ui::console("Writing", TFT_YELLOW);
    uint32_t t0 = millis();
    if (!writer::apply(img, plan, ulog, write_progress, err, sizeof err)) {
        ui::console("WRITE FAILED", TFT_RED);
        ulog("%s", err);
        ulog("Do NOT switch the unit off.");
        if (have_backup) ulog("Its backup: %s", bak);
        ui::status("W retry  R restart unit  ESC end");
        return;
    }
    ui::console("Rescue installed", TFT_GREEN);
    ulog("Done in %lu s. R restarts the unit.", (unsigned long)((millis() - t0) / 1000));
}

static void connected_menu(const jl::Info &info) {
    bool can_rescue = unit.model == engine::MODEL_FM1 || unit.model == engine::MODEL_SMK37;
    for (;;) {
        ui::status(can_rescue ? "ENTER backup W rescue R restart ESC end" : "ENTER backup  R restart unit  ESC end");
        uint32_t last_ping = millis();
        int k = 0;
        while (!(k = ui::read_key())) {
            delay(20);
            if (millis() - last_ping > 1000) {  // keep the session busy
                last_ping = millis();
                if (!engine::keepalive()) {
                    ulog("The unit was disconnected.");
                    return;
                }
            }
        }
        if (k == '\n') {
            backup(info);
            ui::wait_released();
            delay(1500);
        } else if (k == 'w' && !can_rescue) {
            ulog("No rescue image for an %s.", unit.name);
        } else if (k == 'w') {
            rescue(info);
            delay(500);
        } else if (k == 'r') {
            char err[96];
            ulog(jl::restart(err, sizeof err) ? "Restarted: the unit boots its firmware." : "Restart failed");
            return;
        } else if (k == 27) {
            ulog("Left in download mode.");
            return;
        }
    }
}

static void run_session() {
    if (!enter_download_mode()) return;
    ui::status("Connecting...");
    jl::Info info;
    char err[96];
    engine::ConnectResult c = engine::connect(&info, err, sizeof err);
    if (c != engine::CONN_OK) {
        ui::console(c == engine::CONN_LOADER_FAILED ? "Loader failed" : "No connection", TFT_RED);
        ulog("%s", err);
        return;
    }
    ui::console("Connected", TFT_GREEN);
    ulog("Unit: %s (%s)", unit.name, unit.why);
    connected_menu(info);
}

static void end_session() {
    ui::status("Any key: restart the Cardputer");
    ui::wait_released();
    while (!ui::read_key()) delay(20);
    modes::restart_into(modes::ADVANCED);  // the USB host is up: start over clean
}

void run() {
    idle_screen();
    for (;;) {
        // no card at power-on: look again now and then on the idle screen
        static uint32_t last_sd_try;
        if (!sd_ok && millis() - last_sd_try > 3000) {
            last_sd_try = millis();
            if (sd_mount()) ui::status("SD card ready");
        }
        int k = ui::read_key();
        if (k == '\n') {
            ui::wait_released();
            run_session();
            end_session();
        } else if (k == 'h') {
            ui::wait_released();
            run_handoff();
            end_session();
        } else if (k == 'p') {
            int hi, lo;
            usbkey::pin_check(&hi, &lo);
            bool ok = hi == 3 && lo == 0;
            ui::clear_log();
            ui::console(ok ? "Pin check OK" : "Pin check FAILED", ok ? TFT_GREEN : TFT_RED);
            ulog("H%d L%d (expect H3 L0)", hi, lo);
            ulog("Run with nothing plugged into USB-C.");
            ui::status("Any key: back");
            while (!ui::read_key()) delay(20);
            idle_screen();
        } else if (k == 't') {
            ui::clear_log();
            ui::console("Pulse test", TFT_YELLOW);
            ulog("Measuring for 3 s, nothing plugged in.");
            uint32_t n;
            double period;
            if (!usbkey::pulse_test(&n, &period)) {
                ui::console("Pulse test FAILED", TFT_RED);
                ulog("Could not start the pulse generator.");
            } else {
                double ppm = (period - 1000.0) * 1000.0;
                bool ok = n > 2900 && n < 3100 && ppm > -500 && ppm < 500;
                ui::console(ok ? "Pulse test OK" : "Pulse test CHECK", ok ? TFT_GREEN : TFT_ORANGE);
                ulog("%lu edges, period %.3f us", (unsigned long)n, period);
                ulog("error %+.0f ppm (USB FS allows 2500)", ppm);
            }
            ui::status("Any key: back");
            while (!ui::read_key()) delay(20);
            idle_screen();
        } else if (k == 27) {
            ui::wait_released();
            return;
        }
        delay(10);
    }
}

}  // namespace advanced
