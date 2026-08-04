// OpenBricx Deck — OBX serial protocol implementation.
//
// Two things share one newline-delimited channel over USB CDC:
//   1. The OBX discovery handshake: host sends `OBX-WHO`, we reply with
//      `OBX-HELLO <json>` so the Console's serial transport can identify us.
//   2. The legacy Deck control protocol (V/M/T/Q/P/S/N/D/B/I/R/WIPE) used by
//      the Deck plugin to push config and telemetry.

#include "obx_protocol.h"
#include "config.h"
#include "macros.h"
#include "display.h"
#include "input.h"
#include "pinout.h"
#include "battery.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tusb.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_rom_sys.h"

static const char *TAG = "obx";

#define OBX_LINE_MAX 320
static char s_line[OBX_LINE_MAX];
static size_t s_len = 0;

static char s_device_id[13];  // 12 hex chars + NUL

// ── Reboot into the ROM serial bootloader (download mode) ─────────────────────
//
// Setting RTC_CNTL_FORCE_DOWNLOAD_BOOT and resetting makes the ROM stay in
// serial download mode after the restart (the RTC bit survives a software
// reset). This lets the Console re-flash without the user pressing BOOT+RESET:
// the device drops its app USB and re-enumerates as the USB-Serial-JTAG download
// port. Triggered two ways — a 1200-baud CDC "touch" (the esptool/Arduino
// convention) and the OBX `DFU` command.
static void enter_download_mode(void)
{
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_rom_software_reset_system();
}

// TinyUSB calls this when the host changes the CDC line coding. esptool opens
// the port at 1200 baud to request bootloader entry.
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;
    if (coding->bit_rate == 1200) {
        enter_download_mode();
    }
}

// ── CDC write helpers ─────────────────────────────────────────────────────────

static void cdc_write(const char *data, size_t len)
{
    // Use tud_mounted() instead of tud_cdc_connected(): the latter requires DTR
    // to be asserted, which many serial-port libraries skip. tud_mounted() is
    // true once the host has finished USB enumeration — that's sufficient for CDC
    // writes (the data just goes to the host's kernel buffer).
    if (!tud_mounted()) return;
    size_t off = 0;
    while (off < len) {
        uint32_t n = tud_cdc_write(data + off, len - off);
        off += n;
        tud_cdc_write_flush();
        if (n == 0) break;  // host not draining; drop the rest
    }
}

void obx_serial_println(const char *str)
{
    cdc_write(str, strlen(str));
    cdc_write("\n", 1);
}

void obx_emit_pc_action(uint8_t page, uint8_t btn)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "E%u:%u", page, btn);
    obx_serial_println(buf);
}

void obx_emit_profile_sync(uint8_t page)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "PROF_SYNC:%u", page);
    obx_serial_println(buf);
}

// ── Boot / peripheral diagnostics ─────────────────────────────────────────────
//
// The ESP-IDF console (ESP_LOGx) goes to UART0 / USB-Serial-JTAG, neither of
// which is reachable while TinyUSB owns the native USB port. So we mirror the key
// init results to the CDC channel the host already talks to, prefixed `DIAG:`.

static bool s_last_display_ok = false;
static bool s_last_input_ok = false;

void obx_emit_boot_diag(bool display_ok, bool input_ok)
{
    s_last_display_ok = display_ok;
    s_last_input_ok = input_ok;

    char buf[96];

    obx_serial_println("DIAG: --- OpenBricx Deck diagnostics ---");

    snprintf(buf, sizeof(buf), "DIAG: fw=%s chip=%s", OBX_FW_VERSION, OBX_CHIP);
    obx_serial_println(buf);

    snprintf(buf, sizeof(buf), "DIAG: display(ST7789 SPI3 MOSI=%d SCLK=%d DC=%d RST=%d BLK=%d) = %s",
             PIN_TFT_MOSI, PIN_TFT_SCLK, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BLK,
             display_ok ? "OK" : "FAIL");
    obx_serial_println(buf);

    snprintf(buf, sizeof(buf), "DIAG: i2c(SDA=%d SCL=%d) PCF8574@0x%02x = %s",
             PIN_I2C_SDA, PIN_I2C_SCL, PCF8574_ADDR,
             input_ok ? "OK" : "NOT FOUND");
    obx_serial_println(buf);

    // Scan the whole bus so we can tell "wrong address" (e.g. a PCF8574A at 0x38)
    // apart from "nothing on the bus" (wiring / power / pull-ups).
    uint8_t addrs[16];
    int found = input_bus_scan(addrs, sizeof(addrs));
    if (found == 0) {
        obx_serial_println(
            "DIAG: i2c scan: NO devices responded — check the PCF8574 VCC/GND, "
            "that SDA/SCL aren't swapped, and that the module has pull-ups");
    } else {
        int off = snprintf(buf, sizeof(buf), "DIAG: i2c scan found:");
        for (int i = 0; i < found && off < (int)sizeof(buf) - 6; i++) {
            off += snprintf(buf + off, sizeof(buf) - off, " 0x%02x", addrs[i]);
        }
        obx_serial_println(buf);
    }

    // Live read of the expander so we can see whether key/switch/touch lines move.
    uint8_t raw = 0;
    if (input_read_raw(&raw)) {
        snprintf(buf, sizeof(buf),
                 "DIAG: pcf raw=0x%02x  touch(P0)=%d encSw(P7)=%d",
                 raw, (raw & 0x01) ? 1 : 0, (raw & 0x80) ? 0 : 1);
        obx_serial_println(buf);

        // Live matrix scan — hold a key while resending DIAG to see it light up.
        bool keys[9];
        input_debug_scan(keys);
        char m[80];
        int off = snprintf(m, sizeof(m), "DIAG: matrix[1-9]=");
        for (int i = 0; i < 9 && off < (int)sizeof(m) - 2; i++) {
            m[off++] = keys[i] ? '1' : '0';
            if (i < 8) m[off++] = ' ';
        }
        m[off] = '\0';
        obx_serial_println(m);
        obx_serial_println("DIAG: (hold a key/touch and resend DIAG — the matrix digit flips to 1)");
    } else {
        obx_serial_println("DIAG: pcf raw read FAILED (I2C no-ACK) — check SDA/SCL wiring + pull-ups");
    }

    // Battery ADC — the calibration readout. Pair `raw` with a multimeter reading
    // of the cell to set BAT_RAW_MIN/MAX (see pinout.h). `var` >80 means the code
    // thinks no cell is connected (USB-only). Charging detect fires at raw>2430.
    battery_status_t bat = battery_read();
    snprintf(buf, sizeof(buf),
             "DIAG: battery raw=%d var=%d pct=%d charging=%d no_batt=%d",
             bat.raw, bat.variance, bat.percent, bat.charging ? 1 : 0, bat.no_battery ? 1 : 0);
    obx_serial_println(buf);

    obx_serial_println("DIAG: --- end ---");
}

// ── Handshake ─────────────────────────────────────────────────────────────────

static void send_hello(void)
{
    char json[256];
    snprintf(json, sizeof(json),
        "OBX-HELLO {\"obx\":%d,\"product\":\"%s\",\"deviceId\":\"%s\","
        "\"fwVersion\":\"%s\",\"chip\":\"%s\",\"name\":\"%s\",\"hwRev\":\"%s\","
        "\"transports\":[\"serial\"]}",
        OBX_PROTOCOL_VERSION, OBX_PRODUCT, s_device_id,
        OBX_FW_VERSION, OBX_CHIP, OBX_DEVICE_NAME, OBX_HW_REV);
    obx_serial_println(json);
}

// ── Line dispatch ─────────────────────────────────────────────────────────────

// Split "a:b:c" style payloads. Returns count, fills argv with pointers into buf
// (which is mutated in place). Stops at max.
static int split(char *buf, char sep, char **argv, int max)
{
    int n = 0;
    char *p = buf;
    argv[n++] = p;
    while (*p && n < max) {
        if (*p == sep) {
            *p = '\0';
            argv[n++] = p + 1;
        }
        p++;
    }
    return n;
}

static void dispatch(char *line)
{
    if (line[0] == '\0') return;

    // Multi-character word commands are matched first, because some collide with
    // the single-letter prefix commands below — e.g. "DIAG"/"DFU" would otherwise
    // be caught by `case 'D'` (display mode) and never run.
    if (strcmp(line, "OBX-WHO") == 0) { send_hello(); return; }
    if (strcmp(line, "WIPE") == 0) {
        obx_serial_println("WIPING NVS MEMORY...");
        macros_wipe();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return;
    }
    if (strcmp(line, "DFU") == 0) {
        // Reboot into the ROM serial bootloader so the Console can re-flash
        // without a manual BOOT+RESET.
        obx_serial_println("Entering download mode...");
        vTaskDelay(pdMS_TO_TICKS(100));
        enter_download_mode();
        return;
    }
    if (strcmp(line, "DIAG") == 0) {
        // Re-run peripheral diagnostics with a fresh live PCF read.
        obx_emit_boot_diag(s_last_display_ok, input_ready());
        return;
    }

    char cmd = line[0];
    char *body = line + 1;

    switch (cmd) {
    case 'V': {  // V<vol>
        display_sync_volume(atoi(body));
        break;
    }
    case 'M': {  // M<prof>:<btn>:<mode>:<val>[:<mods>]
        char *a[5];
        int n = split(body, ':', a, 5);
        if (n >= 4) {
            macros_update(atoi(a[0]), atoi(a[1]), atoi(a[2]), atoi(a[3]),
                          n >= 5 ? atoi(a[4]) : 0);
        }
        break;
    }
    case 'T': {  // T<prof>:<btn>:<text>
        char *a[3];
        int n = split(body, ':', a, 3);
        if (n >= 3) macros_update_text(atoi(a[0]), atoi(a[1]), a[2]);
        break;
    }
    case 'Q': {  // Q<idx>:<name>
        char *a[2];
        int n = split(body, ':', a, 2);
        if (n >= 2) display_set_profile_name(atoi(a[0]), a[1]);
        break;
    }
    case 'P': {  // P<page>
        int page = atoi(body);
        macros_set_page(page);
        display_show_profile_switch(page);
        break;
    }
    case 'S': {  // S<cpu>:<cTemp>:<rUsed>:<rTotal>:<gpu>:<gTemp>
        char *a[6];
        int n = split(body, ':', a, 6);
        if (n >= 1) {
            display_sync_sysinfo(
                atoi(a[0]),
                n > 1 ? atoi(a[1]) : 0,
                n > 2 ? atof(a[2]) : 0,
                n > 3 ? atof(a[3]) : 0,
                n > 4 ? atoi(a[4]) : 0,
                n > 5 ? atoi(a[5]) : 0);
        }
        break;
    }
    case 'N': {  // N<title>|<artist>|<pos>|<dur>|<playing>
        char *a[5];
        int n = split(body, '|', a, 5);
        if (n >= 5) {
            display_show_now_playing(a[0], a[1], atol(a[2]), atol(a[3]), atoi(a[4]) == 1);
        }
        break;
    }
    case 'D':  // D<mode>
        display_set_mode(atoi(body));
        break;
    case 'B':  // B<1-100>
        display_set_brightness(atoi(body));
        break;
    case 'I':  // version query
        obx_serial_println("VER:" OBX_FW_VERSION);
        break;
    case 'R':  // reboot
        obx_serial_println("Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        break;
    default:
        // Unknown single-letter command — ignore. (Word commands like WIPE/DFU/
        // DIAG are handled above, before this switch.)
        break;
    }
}

// ── Polling ───────────────────────────────────────────────────────────────────

void obx_poll(void)
{
    if (!tud_cdc_available()) return;

    uint8_t buf[64];
    uint32_t count = tud_cdc_read(buf, sizeof(buf));
    for (uint32_t i = 0; i < count; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r') {
            if (s_len > 0) {
                s_line[s_len] = '\0';
                dispatch(s_line);
                s_len = 0;
            }
        } else if (s_len < OBX_LINE_MAX - 1) {
            s_line[s_len++] = c;
        } else {
            s_len = 0;  // overflow guard
        }
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────

void obx_init(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "OBX protocol ready, deviceId=%s", s_device_id);
}
