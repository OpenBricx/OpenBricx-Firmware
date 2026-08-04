// OpenBricx Deck — application entry point + main input/protocol task.
//
// Mirrors the original Arduino loop(): drain the OBX serial channel, scan the
// button matrix, handle the encoder (rotation + click gestures) and touch pad,
// and sample the battery periodically.

#include "config.h"
#include "pinout.h"
#include "usb_descriptors.h"
#include "hid.h"
#include "macros.h"
#include "obx_protocol.h"
#include "input.h"
#include "encoder.h"
#include "battery.h"
#include "display.h"
#include "ble_hid.h"
#include "wifi.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "deck";

// Consumer-control usage codes for volume.
#define USAGE_VOL_UP   0xE9
#define USAGE_VOL_DOWN 0xEA

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ── Encoder click gestures: single / double / long ────────────────────────────
static void handle_encoder_switch(void)
{
    static bool pressing = false;
    static uint32_t press_start = 0;
    static uint32_t last_release = 0;
    static int click_count = 0;

    bool down = input_encoder_switch();
    uint32_t now = now_ms();

    if (down && !pressing) {
        pressing = true;
        press_start = now;
    } else if (!down && pressing) {
        pressing = false;
        last_release = now;
        uint32_t held = now - press_start;
        if (held > 600) {
            // Long press → next profile, and tell the host.
            macros_next_page();
            display_show_profile_switch(macros_get_page());
            obx_emit_profile_sync(macros_get_page());
            click_count = 0;
        } else {
            click_count++;
        }
    }

    // Resolve pending clicks once the double-click window closes.
    if (click_count > 0 && (now - last_release > 350)) {
        if (click_count >= 2) {
            display_toggle_settings();   // double-click opens/closes the settings menu
        } else if (display_settings_open()) {
            display_menu_action();       // single-click inside settings = enter/leave submenu
        } else {
            macros_fire(9);              // encoder single-click = logical button 10
        }
        click_count = 0;
    }
}

// ── Rotary rotation: volume ───────────────────────────────────────────────────
static void handle_encoder_rotation(void)
{
    int delta = encoder_take_delta();
    if (delta == 0) return;

    // When the settings menu is open the knob adjusts the highlighted item
    // instead of volume.
    if (display_settings_open()) {
        display_menu_scroll(delta > 0 ? 1 : -1);
        return;
    }

    bool up = delta > 0;
    uint16_t usage = up ? USAGE_VOL_UP : USAGE_VOL_DOWN;
    int steps = up ? delta : -delta;
    for (int i = 0; i < steps; i++) {
        hid_media_press(usage);
        hid_media_release();
    }
    display_show_volume(up);   // pop the on-device volume HUD
}

// ── Touch pad: double-tap cycles the display mode ─────────────────────────────
static void handle_touch(void)
{
    static bool pressing = false;
    static uint32_t touch_start = 0;
    static uint32_t last_release = 0;
    static int tap_count = 0;

    bool touched = input_touch();
    uint32_t now = now_ms();

    if (touched && !pressing) {
        pressing = true;
        touch_start = now;
    } else if (!touched && pressing) {
        pressing = false;
        last_release = now;
        uint32_t held = now - touch_start;
        if (held > 500) {
            tap_count = 0;  // end of a hold, not a tap
        } else if (held > 20) {
            tap_count++;
        }
    }

    if (tap_count > 0 && (now - last_release > 350)) {
        // Only double-tap acts (Mochi <-> Stats). A single tap is deliberately
        // inert — it used to cycle emote previews, which fired on every stray
        // brush of the pad.
        if (tap_count >= 2) {
            display_cycle_mode();
        }
        tap_count = 0;
    }
}

// ── Update mode → Wi-Fi AP ────────────────────────────────────────────────────
// The on-device Settings → Update screen is the trigger: while it's open the deck
// hosts its SoftAP (for OTA); leaving the screen takes it back down.
static void handle_update_mode(void)
{
    static bool last = false;
    bool want = display_update_mode();
    if (want == last) return;
    last = want;
    if (want) {
        display_backlight_off();   // shed the biggest load before the RF inrush
        wifi_ap_start();           // synchronous — the brownout-critical bring-up is here
        display_backlight_dim();   // AP is up: keep it dim through the OTA transfer
    } else {
        wifi_ap_stop();
        display_backlight_restore();   // back to the user's saved brightness
    }
}

// ── Pairing mode → open BLE advertising ───────────────────────────────────────
// The Settings → Pairing screen is the trigger: while it's open the deck advertises
// openly and accepts a new Bluetooth bond; leaving the screen drops back to
// bonded-only (whitelist) advertising, so it can't be paired by accident.
static void handle_pairing_mode(void)
{
    static bool last = false;
    bool want = display_pairing_mode();
    if (want == last) return;
    last = want;
    ble_hid_set_pairing(want);
}

// ── CPU frequency vs power source ─────────────────────────────────────────────
// 240 MHz on USB (the 5 V rail has headroom); 160 MHz on battery, where the
// AMS1117 LDO can't cleanly source the 240 MHz load from a single 3.7 V cell.
// Runtime DFS via the PM framework (CONFIG_PM_ENABLE) — light sleep stays off, so
// max == min just pins a fixed frequency we can flip at runtime.
static void apply_cpu_freq(bool on_usb)
{
    static int last = -1;
    int mhz = on_usb ? 240 : 160;
    if (mhz == last) return;
    last = mhz;
    esp_pm_config_t pm = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,
    };
    if (esp_pm_configure(&pm) == ESP_OK)
        ESP_LOGI(TAG, "CPU %d MHz (%s)", mhz, on_usb ? "USB" : "battery");
}

// ── Battery (every 10 s) ──────────────────────────────────────────────────────
static void handle_battery(void)
{
    static uint32_t last = 0;
    uint32_t now = now_ms();
    if (last != 0 && now - last < DECK_BATTERY_PERIOD_MS) return;
    last = now;

    battery_status_t b = battery_read();
    display_update_battery(b.percent, b.charging, b.no_battery);
    ble_hid_set_battery((uint8_t)b.percent);

    bool on_usb = b.no_battery || b.charging;
    apply_cpu_freq(on_usb);   // USB present → 240; on battery → 160

    // Backlight follows the power source: full saved brightness on USB, ~25% on
    // battery (saves the rail + runtime). On the very first poll this also lights
    // the backlight, which boots dark (duty 0) to keep its load off the rail during
    // the brownout-critical cold-start window. Only re-applies on a source change.
    static int last_src = -1;
    if ((int)on_usb != last_src) {
        last_src = on_usb;
        display_set_power_source(!on_usb);
    }
}

// Init results handed from app_main to the deck task for the boot diagnostics.
// (Emitting the DIAG block from app_main overflowed its 3.5 KB stack; the deck
// task has 6 KB, so we report from there on the first iteration.)
typedef struct {
    bool display_ok;
    bool input_ok;
} deck_boot_status_t;

// ── Main task ─────────────────────────────────────────────────────────────────
static void deck_task(void *arg)
{
    deck_boot_status_t boot = *(deck_boot_status_t *)arg;
    esp_task_wdt_add(NULL);

    // Let the host attach to the CDC port, then mirror peripheral status to it
    // (the ESP_LOGx console isn't reachable while TinyUSB owns USB).
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_task_wdt_reset();
    obx_emit_boot_diag(boot.display_ok, boot.input_ok);

    // Discard encoder counts accumulated during boot / USB enumeration. Power-up
    // GPIO transitions on CLK/DT pile up in the PCNT before the loop starts; if we
    // didn't flush them, the first encoder_take_delta() would replay the whole
    // burst as volume changes and slam the PC volume to 0.
    encoder_take_delta();

    bool pressed[9];
    while (1) {
        esp_task_wdt_reset();

        obx_poll();

        input_scan_matrix(pressed);
        for (int i = 0; i < 9; i++) {
            if (pressed[i]) macros_fire(i);
        }

        handle_encoder_rotation();
        handle_encoder_switch();
        handle_touch();
        handle_battery();
        handle_update_mode();
        handle_pairing_mode();
        display_set_ble_connected(ble_hid_connected());   // Stats-screen BT icon

        vTaskDelay(pdMS_TO_TICKS(DECK_TASK_PERIOD_MS));
    }
}

void app_main(void)
{
    // NVS first — macros and config live here.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "OpenBricx Deck %s booting", OBX_FW_VERSION);

    // USB + HID + protocol.
    usb_init();
    hid_init();
    obx_init();
    ble_hid_init();

    // Macros (depends on NVS + HID + protocol for emitting events).
    macros_init();

    // Peripherals. Status is reported over CDC from the deck task (see deck_task).
    // `static` so it outlives app_main and stays valid for the task to read.
    static deck_boot_status_t boot;
    boot.display_ok = display_init();
    boot.input_ok = input_init();
    if (!boot.input_ok) {
        ESP_LOGE(TAG, "input init failed — check PCF8574 wiring");
    }
    encoder_init();
    battery_init();

    // Pick the CPU frequency from the actual power source before the heavier work
    // runs (240 MHz on USB, 160 MHz on battery). The battery poll re-checks every 10 s.
    {
        battery_status_t b0 = battery_read();
        apply_cpu_freq(b0.no_battery || b0.charging);
    }

    // Wi-Fi stack for firmware OTA. The radio stays off until the user opens
    // Settings → Update on the device, which brings up the SoftAP (see
    // handle_update_mode).
    wifi_init();

    display_start();   // render task on core 1 (separate from input/USB)

    xTaskCreate(deck_task, "deck", 6144, &boot, 5, NULL);
    ESP_LOGI(TAG, "deck task started");
}
