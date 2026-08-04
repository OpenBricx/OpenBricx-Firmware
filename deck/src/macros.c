// OpenBricx Deck — NVS macro resolver.
//
// Key layout inside the namespace:
//   p<page>_b<btn>_mode  (u8)   p<page>_b<btn>_val (u16)
//   p<page>_b<btn>_mod   (u8)   p<page>_b<btn>_txt (str)
//   active_page          (u8)
//
// NOTE (fw 1.8.0): the namespace was renamed off the pre-OpenBricx name. A deck
// updated from <=1.7.6 therefore finds an empty namespace and re-seeds the
// built-in Profile-0 defaults; the Console re-pushes the user's config on the
// next connect (syncAll). NVS namespaces are capped at 15 chars.

#include "macros.h"
#include "config.h"
#include "hid.h"
#include "obx_protocol.h"

#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "macros";
static const char *NS = "obx_deck_cfg";

static uint8_t s_page = 0;

// Modifier bits for MODE_HOTKEY (match the plugin: bit0 Ctrl … bit3 Gui).
#define MOD_CTRL 0x01
#define MOD_GUI  0x08

// Built-in Profile-0 layout, seeded on a never-configured device so the Deck is
// useful standalone (before it's ever connected to the Console). Mirrors the
// Console's DEFAULT_PRESETS so a fresh device and a fresh Console config agree.
typedef struct {
    uint8_t btn, mode, mods;
    uint16_t val;
} default_macro_t;

static const default_macro_t DEFAULTS[] = {
    {1, MODE_MEDIA,  0, 182},          // Previous
    {2, MODE_MEDIA,  0, 205},          // Play/Pause
    {3, MODE_MEDIA,  0, 181},          // Next
    {4, MODE_HOTKEY, MOD_CTRL,  99},   // Copy   (Ctrl+C)
    {5, MODE_HOTKEY, MOD_CTRL, 118},   // Paste  (Ctrl+V)
    {6, MODE_HOTKEY, MOD_CTRL, 122},   // Undo   (Ctrl+Z)
    {7, MODE_MEDIA,  0, 226},          // Mute
    {8, MODE_HOTKEY, MOD_GUI, 0xF046}, // Screenshot (Win+PrintScreen; 0xF046 = HID PrtSc)
    {9, MODE_HOTKEY, MOD_GUI,  108},   // Lock PC    (Win+L)
};

static void make_key(char *out, size_t sz, uint8_t page, uint8_t btn, const char *suffix)
{
    snprintf(out, sz, "p%u_b%u_%s", page, btn, suffix);
}

static nvs_handle_t open_rw(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return 0;
    return h;
}

void macros_init(void)
{
    nvs_handle_t h = open_rw();
    if (!h) {
        ESP_LOGE(TAG, "failed to open NVS namespace");
        return;
    }

    uint8_t page = 0;
    nvs_get_u8(h, "active_page", &page);
    s_page = (page < DECK_MAX_PAGES) ? page : 0;

    // Seed the full default layout if the device has never been configured (we
    // probe P0/B1's mode key as the "ever configured" marker). macros_update opens
    // its own handle, so close this one first.
    char key[24];
    make_key(key, sizeof(key), 0, 1, "mode");
    uint8_t mode;
    if (nvs_get_u8(h, key, &mode) != ESP_OK) {
        nvs_close(h);
        for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
            macros_update(0, DEFAULTS[i].btn, DEFAULTS[i].mode, DEFAULTS[i].val, DEFAULTS[i].mods);
        }
        ESP_LOGI(TAG, "seeded %d default macros for P0",
                 (int)(sizeof(DEFAULTS) / sizeof(DEFAULTS[0])));
        return;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded config, active page %u", s_page);
}

uint8_t macros_get_page(void) { return s_page; }

void macros_set_page(uint8_t page)
{
    if (page >= DECK_MAX_PAGES) page = 0;
    s_page = page;
    nvs_handle_t h = open_rw();
    if (h) {
        nvs_set_u8(h, "active_page", page);
        nvs_commit(h);
        nvs_close(h);
    }
}

void macros_next_page(void)
{
    macros_set_page((s_page + 1) % DECK_MAX_PAGES);
}

void macros_fire(uint8_t btn_index)
{
    uint8_t btn = btn_index + 1;  // stored 1-based
    char mkey[24], vkey[24], modkey[24], tkey[24];
    make_key(mkey, sizeof(mkey), s_page, btn, "mode");
    make_key(vkey, sizeof(vkey), s_page, btn, "val");
    make_key(modkey, sizeof(modkey), s_page, btn, "mod");
    make_key(tkey, sizeof(tkey), s_page, btn, "txt");

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t mode = MODE_NONE;
    uint16_t val = 0;
    nvs_get_u8(h, mkey, &mode);
    nvs_get_u16(h, vkey, &val);

    switch (mode) {
    case MODE_KEYBOARD:
        hid_tap_ascii((uint8_t)val);
        break;

    case MODE_MEDIA:
        hid_media_press(val);
        vTaskDelay(pdMS_TO_TICKS(35));
        hid_media_release();
        break;

    case MODE_HOTKEY: {
        uint8_t mods = 0;
        nvs_get_u8(h, modkey, &mods);
        hid_hotkey(val, mods);  // val may be an ASCII char or a 0xF0xx special key
        break;
    }

    case MODE_TEXT: {
        char text[DECK_MAX_TEXT_LEN + 1] = {0};
        size_t len = sizeof(text);
        if (nvs_get_str(h, tkey, text, &len) == ESP_OK && text[0]) {
            hid_type_text(text);
        }
        break;
    }

    case MODE_LAUNCH_APP:
    case MODE_OPEN_LINK:
        // Host executes these — emit the event for the companion to handle.
        // (The original firmware only emitted for mode 5; mode 6 was a latent
        // no-op. We emit for both so Open Link works.)
        obx_emit_pc_action(s_page, btn);
        break;

    default:
        break;
    }

    nvs_close(h);
}

void macros_update(uint8_t prof, uint8_t btn, uint8_t mode, uint16_t val, uint8_t mods)
{
    nvs_handle_t h = open_rw();
    if (!h) return;

    char key[24];
    make_key(key, sizeof(key), prof, btn, "mode");
    nvs_set_u8(h, key, mode);
    make_key(key, sizeof(key), prof, btn, "val");
    nvs_set_u16(h, key, val);
    if (mode == MODE_HOTKEY) {
        make_key(key, sizeof(key), prof, btn, "mod");
        nvs_set_u8(h, key, mods);
    }
    nvs_commit(h);
    nvs_close(h);
}

void macros_update_text(uint8_t prof, uint8_t btn, const char *text)
{
    nvs_handle_t h = open_rw();
    if (!h) return;

    char key[24];
    make_key(key, sizeof(key), prof, btn, "txt");
    nvs_set_str(h, key, text);
    make_key(key, sizeof(key), prof, btn, "mode");
    nvs_set_u8(h, key, MODE_TEXT);
    nvs_commit(h);
    nvs_close(h);
}

void macros_wipe(void)
{
    nvs_handle_t h = open_rw();
    if (h) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "NVS config wiped");
}
