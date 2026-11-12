// USB host + Bulk-Only Transport, after M-Flasher's src/usb.rs (MIT).
#include "uboot_host.h"

#include <initializer_list>
#include <string.h>
#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <usb/usb_host.h>
#include <esp_log.h>
#include <soc/gpio_reg.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>
#include <soc/usb_wrap_reg.h>

namespace host {

static constexpr uint32_t CBW_SIGNATURE = 0x43425355;  // "USBC"
static constexpr uint32_t CSW_SIGNATURE = 0x53425355;  // "USBS"
static constexpr uint32_t TIMEOUT_MS = 5000;           // M-Flasher: 10 s; a 64 KiB erase takes well under 1 s
static constexpr size_t OUT_SIZE = 4096;               // CBW or up to 4 KiB of data
static constexpr size_t IN_SIZE = 16384;               // the loader's largest read

static usb_host_client_handle_t client;
static usb_device_handle_t dev;
static volatile int new_addr = -1;
static volatile bool gone;
static uint8_t ep_in, ep_out, iface;
static uint16_t mps_in = 64;
static usb_transfer_t *x_out, *x_in, *x_ctl;
static SemaphoreHandle_t done;
static uint32_t tag;
static uint8_t dev_addr;
static Log trace;  // the app's log, for every transfer that is not plainly OK

// ---------------------------------------------------------------- diagnostics

// ESP-IDF's log lines (errors only in this build) kept for the app's log:
// they may come from any task, the SD card is written from the main one.
static portMUX_TYPE idf_mux = portMUX_INITIALIZER_UNLOCKED;
static char idf_buf[8192];
static size_t idf_len;

static int idf_vprintf(const char *fmt, va_list ap) {
    char line[160];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n <= 0) return n;
    size_t k = (size_t)n < sizeof line ? n : sizeof line - 1;
    portENTER_CRITICAL_SAFE(&idf_mux);
    if (idf_len + k < sizeof idf_buf) {
        memcpy(idf_buf + idf_len, line, k);
        idf_len += k;
    }
    portEXIT_CRITICAL_SAFE(&idf_mux);
    return n;
}

static void drain_idf_log(Log log) {
    static char copy[sizeof idf_buf + 1];
    portENTER_CRITICAL(&idf_mux);
    size_t n = idf_len;
    memcpy(copy, idf_buf, n);
    idf_len = 0;
    portEXIT_CRITICAL(&idf_mux);
    copy[n] = 0;
    for (char *line = strtok(copy, "\r\n"); line; line = strtok(nullptr, "\r\n")) log("IDF: %s", line);
}

static constexpr uint32_t OTG_GINTSTS = 0x60080014;
static constexpr uint32_t OTG_HPRT = 0x60080440;

// The root port as the OTG controller sees it, and the pads as the GPIOs see them.
static void port_state(char *out, size_t n) {
    uint32_t h = REG_READ(OTG_HPRT), in = REG_READ(GPIO_IN_REG);
    snprintf(out, n, "port conn%lu ena%lu pwr%lu line D+%lu D-%lu spd%lu | pads D+%d D-%d",
             (unsigned long)(h & 1), (unsigned long)((h >> 2) & 1), (unsigned long)((h >> 12) & 1),
             (unsigned long)((h >> 10) & 1), (unsigned long)((h >> 11) & 1), (unsigned long)((h >> 17) & 3),
             (in >> 20) & 1, (in >> 19) & 1);
}

// Host channels in use: characteristics (odd-frame bit masked), transfer size
// and pending interrupts. Changes show whether a transfer moves at all.
static void channel_state(char *out, size_t n) {
    size_t k = 0;
    out[0] = 0;
    for (int ch = 0; ch < 8 && k < n; ch++) {
        uint32_t base = 0x60080500 + ch * 0x20;
        uint32_t c = REG_READ(base);
        if (!(c & 0x80000000)) continue;  // ChEna
        k += snprintf(out + k, n - k, "ch%d %08lX %08lX %03lX ", ch, (unsigned long)(c & ~(1ul << 29)),
                      (unsigned long)REG_READ(base + 0x10), (unsigned long)(REG_READ(base + 0x08) & 0x7FF));
    }
    if (!out[0]) snprintf(out, n, "no channel active");
}

static void log_phy(Log log) {
    log("PHY: rtc usb_conf %08lX wrap %08lX usj %08lX gintsts %08lX", (unsigned long)REG_READ(RTC_CNTL_USB_CONF_REG),
        (unsigned long)REG_READ(USB_WRAP_OTG_CONF_REG), (unsigned long)REG_READ(USB_SERIAL_JTAG_CONF0_REG),
        (unsigned long)REG_READ(OTG_GINTSTS));
}

static void lib_task(void *) {
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

static void client_cb(const usb_host_client_event_msg_t *m, void *) {
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) new_addr = m->new_dev.address;
    else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) gone = true;
}

static void client_task(void *) {
    for (;;) usb_host_client_handle_events(client, portMAX_DELAY);
}

static void xfer_cb(usb_transfer_t *) { xSemaphoreGive(done); }

// Arduino's ESP-IDF has CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK set, and
// with that option ESP-IDF cancels every device unless a filter accepts it.
static bool enum_filter(const usb_device_desc_t *, uint8_t *bConfigurationValue) {
    *bConfigurationValue = 1;
    return true;
}

bool start() {
    esp_log_set_vprintf(idf_vprintf);
    // ESP-IDF's errors, plus the USB host's own steps where the build has them
    esp_log_level_set("*", ESP_LOG_ERROR);
    for (const char *tag : {"ENUM", "HUB", "USBH", "USB HOST", "HCD DWC", "EXT_PORT", "EXT_HUB"})
        esp_log_level_set(tag, ESP_LOG_DEBUG);
    ESP_LOGE("fm1", "log capture works");    // shows that ESP-IDF's errors reach our log
    usb_host_config_t cfg = {};
    cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.enum_filter_cb = enum_filter;
    if (usb_host_install(&cfg) != ESP_OK) return false;
    xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, nullptr, 6, nullptr, 0);

    usb_host_client_config_t cc = {};
    cc.is_synchronous = false;
    cc.max_num_event_msg = 5;
    cc.async.client_event_callback = client_cb;
    cc.async.callback_arg = nullptr;
    if (usb_host_client_register(&cc, &client) != ESP_OK) return false;
    xTaskCreatePinnedToCore(client_task, "usb_client", 4096, nullptr, 5, nullptr, 0);

    done = xSemaphoreCreateBinary();
    return usb_host_transfer_alloc(OUT_SIZE, 0, &x_out) == ESP_OK &&
           usb_host_transfer_alloc(IN_SIZE, 0, &x_in) == ESP_OK &&
           usb_host_transfer_alloc(64, 0, &x_ctl) == ESP_OK;
}

bool connected() { return dev && !gone; }

// Walks the active configuration for the first interface with a bulk IN and
// a bulk OUT endpoint.
static bool find_bulk(const usb_config_desc_t *cd) {
    const uint8_t *p = (const uint8_t *)cd;
    int total = cd->wTotalLength, cur = -1, found_if = -1;
    uint8_t in = 0, out = 0;
    uint16_t mps = 64;
    for (int i = 0; i + 1 < total && p[i] >= 2; i += p[i]) {
        if (p[i + 1] == 0x04) {  // interface
            if (found_if < 0) in = out = 0;
            cur = p[i + 2];
        } else if (p[i + 1] == 0x05 && found_if < 0 && (p[i + 3] & 3) == 2) {  // bulk endpoint
            if (p[i + 2] & 0x80) {
                in = p[i + 2];
                mps = p[i + 4] | (p[i + 5] << 8);
            } else {
                out = p[i + 2];
            }
            if (in && out) found_if = cur;
        }
    }
    if (found_if < 0) return false;
    iface = found_if;
    ep_in = in;
    ep_out = out;
    mps_in = mps ? mps : 64;
    return true;
}

bool wait_device(uint32_t timeout_ms, uint16_t *vid, uint16_t *pid, Log log, char *err, size_t errlen) {
    TickType_t t0 = xTaskGetTickCount();
    char last[96] = "", now[96], last_ch[160] = "", now_ch[160];
    int ch_logged = 0;
    TickType_t last_fn = t0;
    log_phy(log);
    while (new_addr < 0) {
        unsigned long ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        port_state(now, sizeof now);
        if (strcmp(now, last) != 0) {
            log("%5lu ms %s", ms, now);
            strcpy(last, now);
        }
        channel_state(now_ch, sizeof now_ch);
        if (strcmp(now_ch, last_ch) != 0 && ch_logged < 40) {
            log("%5lu ms %s", ms, now_ch);
            strcpy(last_ch, now_ch);
            ch_logged++;
        }
        if (xTaskGetTickCount() - last_fn > pdMS_TO_TICKS(2000)) {
            last_fn = xTaskGetTickCount();
            log("%5lu ms frame %04lX gintsts %08lX haint %02lX", ms, (unsigned long)(REG_READ(0x60080408) & 0xFFFF),
                (unsigned long)REG_READ(OTG_GINTSTS), (unsigned long)REG_READ(0x60080414));
        }
        drain_idf_log(log);
        if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(timeout_ms)) {
            log_phy(log);
            snprintf(err, errlen, "no USB device in %lu ms", (unsigned long)timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    drain_idf_log(log);
    trace = log;
    if (usb_host_device_open(client, (uint8_t)new_addr, &dev) != ESP_OK) {
        snprintf(err, errlen, "could not open the USB device");
        return false;
    }
    usb_device_info_t di;
    if (usb_host_device_info(dev, &di) == ESP_OK) dev_addr = di.dev_addr;
    const usb_device_desc_t *dd;
    usb_host_get_device_descriptor(dev, &dd);
    *vid = dd->idVendor;
    *pid = dd->idProduct;
    const usb_config_desc_t *cd;
    if (usb_host_get_active_config_descriptor(dev, &cd) != ESP_OK || !find_bulk(cd)) {
        snprintf(err, errlen, "%04X:%04X has no bulk interface", *vid, *pid);
        return false;
    }
    if (usb_host_interface_claim(client, dev, iface, 0) != ESP_OK) {
        snprintf(err, errlen, "could not claim interface %u", iface);
        return false;
    }
    return true;
}

// Runs one transfer and waits for it. A transfer that times out is halted,
// flushed (its callback then runs as canceled) and cleared.
static usb_transfer_status_t run(usb_transfer_t *t, uint8_t ep, int n) {
    t->device_handle = dev;
    t->bEndpointAddress = ep;
    t->num_bytes = n;
    t->callback = xfer_cb;
    t->context = nullptr;
    xSemaphoreTake(done, 0);
    if (usb_host_transfer_submit(t) != ESP_OK) return USB_TRANSFER_STATUS_ERROR;
    if (xSemaphoreTake(done, pdMS_TO_TICKS(TIMEOUT_MS)) != pdTRUE) {
        usb_host_endpoint_halt(dev, ep);
        usb_host_endpoint_flush(dev, ep);
        xSemaphoreTake(done, pdMS_TO_TICKS(1000));
        usb_host_endpoint_clear(dev, ep);
        return USB_TRANSFER_STATUS_TIMED_OUT;
    }
    return t->status;
}

static bool control(uint8_t type, uint8_t req, uint16_t value, uint16_t index) {
    usb_setup_packet_t *s = (usb_setup_packet_t *)x_ctl->data_buffer;
    s->bmRequestType = type;
    s->bRequest = req;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = 0;
    x_ctl->device_handle = dev;
    x_ctl->bEndpointAddress = 0;
    x_ctl->num_bytes = sizeof(usb_setup_packet_t);
    x_ctl->callback = xfer_cb;
    xSemaphoreTake(done, 0);
    if (usb_host_transfer_submit_control(client, x_ctl) != ESP_OK) return false;
    if (xSemaphoreTake(done, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    return x_ctl->status == USB_TRANSFER_STATUS_COMPLETED;
}

// The data toggle of the host channel that serves ep, back to DATA0. A
// CLEAR_FEATURE(ENDPOINT_HALT) resets the device's toggle; ESP-IDF's
// usb_host_endpoint_clear() does not reset the host's (hcd_dwc.c
// _pipe_cmd_clear), which keeps it in HCTSIZ.PID (bits 29-30) between
// transfers. Out of step, each side drops the other's packets as repeats.
static void reset_toggle(uint8_t ep) {
    for (int ch = 0; ch < 8; ch++) {
        uint32_t base = 0x60080500 + ch * 0x20, c = REG_READ(base);
        bool match = ((c >> 11) & 0xF) == (ep & 0xF) && ((c >> 15) & 1) == (ep >> 7) &&
                     ((c >> 22) & 0x7F) == dev_addr && ((c >> 18) & 3) == 2;  // bulk
        if (match && !(c & 0x80000000)) REG_CLR_BIT(base + 0x10, 3u << 29);
    }
}

// CLEAR_FEATURE(ENDPOINT_HALT) on the device, then the host side of the pipe,
// its data toggle included.
static void clear_halt(uint8_t ep) {
    bool ok = control(0x02, 0x01, 0, ep);
    usb_host_endpoint_halt(dev, ep);
    usb_host_endpoint_flush(dev, ep);
    usb_host_endpoint_clear(dev, ep);
    reset_toggle(ep);
    if (trace) trace("USB: cleared halt on EP %02X%s", ep, ok ? "" : " (CLEAR_FEATURE failed)");
}

// Bulk-Only Mass Storage Reset, then both pipes cleared (BOT 5.3.4).
static void reset_recovery() {
    bool ok = control(0x21, 0xFF, 0, iface);
    if (trace) trace("USB: mass storage reset%s", ok ? "" : " failed");
    clear_halt(ep_in);
    clear_halt(ep_out);
}

static bool is_csw(const uint8_t *b, uint32_t *t) {
    uint32_t sig;
    memcpy(&sig, b, 4);
    if (sig != CSW_SIGNATURE) return false;
    memcpy(t, b + 4, 4);
    return true;
}

static Result transfer(const uint8_t *cdb, uint8_t cdb_len, uint8_t *buf, uint32_t len, bool in,
                       uint32_t *moved, char *err, size_t errlen) {
    *moved = 0;
    if (!connected()) {
        snprintf(err, errlen, "the unit is not connected");
        return FAILED;
    }
    tag++;
    uint8_t *c = x_out->data_buffer;
    memset(c, 0, 31);
    memcpy(c, &CBW_SIGNATURE, 4);
    memcpy(c + 4, &tag, 4);
    memcpy(c + 8, &len, 4);
    c[12] = in ? 0x80 : 0x00;
    c[13] = 0;  // LUN
    c[14] = cdb_len;
    memcpy(c + 15, cdb, cdb_len);
    if (run(x_out, ep_out, 31) != USB_TRANSFER_STATUS_COMPLETED) {
        snprintf(err, errlen, "cmd %02X%02X: sending it", cdb[0], cdb[1]);
        return FAILED;
    }

    uint8_t csw[13];
    bool early = false;
    if (len) {
        if (in) {
            int want = (len + mps_in - 1) / mps_in * mps_in;
            for (int skipped = 0;;) {
                usb_transfer_status_t st = run(x_in, ep_in, want);
                if (st == USB_TRANSFER_STATUS_STALL) {
                    if (trace) trace("USB: cmd %02X%02X data IN stalled", cdb[0], cdb[1]);
                    clear_halt(ep_in);  // stalled data phase: the status follows
                    break;
                }
                if (st != USB_TRANSFER_STATUS_COMPLETED) {
                    snprintf(err, errlen, "cmd %02X%02X: reading data (%d)", cdb[0], cdb[1], (int)st);
                    return FAILED;
                }
                uint32_t n = x_in->actual_num_bytes, t;
                // 13 bytes in place of more asked for may be a status block
                if (n == 13 && len > 13 && is_csw(x_in->data_buffer, &t)) {
                    if (t == tag) {
                        memcpy(csw, x_in->data_buffer, 13);
                        early = true;
                        break;
                    }
                    if (skipped++ < 2) continue;  // an earlier command's status, left over
                }
                *moved = n < len ? n : len;
                memcpy(buf, x_in->data_buffer, *moved);
                break;
            }
        } else {
            memcpy(x_out->data_buffer, buf, len);
            usb_transfer_status_t st = run(x_out, ep_out, len);
            if (st == USB_TRANSFER_STATUS_STALL) {
                if (trace) trace("USB: cmd %02X%02X data OUT stalled", cdb[0], cdb[1]);
                clear_halt(ep_out);
            } else if (st != USB_TRANSFER_STATUS_COMPLETED) {
                snprintf(err, errlen, "cmd %02X%02X: sending data (%d)", cdb[0], cdb[1], (int)st);
                return FAILED;
            } else {
                *moved = len;
            }
        }
    }
    if (!early) {
        usb_transfer_status_t st = run(x_in, ep_in, mps_in);
        if (st == USB_TRANSFER_STATUS_STALL) {
            if (trace) trace("USB: cmd %02X%02X status stalled", cdb[0], cdb[1]);
            clear_halt(ep_in);
            st = run(x_in, ep_in, mps_in);
        }
        if (st != USB_TRANSFER_STATUS_COMPLETED || x_in->actual_num_bytes != 13) {
            snprintf(err, errlen, "cmd %02X%02X: reading status (%d)", cdb[0], cdb[1], (int)st);
            return FAILED;
        }
        memcpy(csw, x_in->data_buffer, 13);
    }
    uint32_t t;
    if (!is_csw(csw, &t) || t != tag) {
        snprintf(err, errlen, "cmd %02X%02X: invalid status block", cdb[0], cdb[1]);
        return FAILED;
    }
    if (csw[12] == 0) return OK;
    if (csw[12] == 1) {
        snprintf(err, errlen, "cmd %02X%02X refused (status 1)", cdb[0], cdb[1]);
        return REFUSED;
    }
    snprintf(err, errlen, "cmd %02X%02X: phase error (status %u)", cdb[0], cdb[1], csw[12]);
    return FAILED;
}

Result execute(const uint8_t *cdb, uint8_t cdb_len, uint8_t *buf, uint32_t len, bool in,
               uint32_t *moved, char *err, size_t errlen) {
    Result r = transfer(cdb, cdb_len, buf, len, in, moved, err, errlen);
    if (r != OK && trace) trace("USB: %s: %s", r == REFUSED ? "refused" : "FAILED", err);
    // a refused command leaves the transport in order; a failed one is reset
    if (r == FAILED && connected()) reset_recovery();
    return r;
}

}  // namespace host
