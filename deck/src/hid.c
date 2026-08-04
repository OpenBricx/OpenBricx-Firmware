// OpenBricx Deck — HID output: USB HID with BLE HID fallback.

#include "hid.h"
#include "usb_descriptors.h"
#include "ble_hid.h"

#include "tusb.h"
#include "class/hid/hid.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// HID modifier byte bits happen to match our macro modifier bitmask exactly
// (Ctrl=1, Shift=2, Alt=4, Gui=8), so no remapping is needed.
#define KBD_MOD_SHIFT 0x02

// ASCII -> {needs_shift, HID keycode}. Standard TinyUSB conversion table,
// trimmed to the printable range plus the common control chars.
static const uint8_t ascii2kc[128][2] = {
    {0, 0},        {0, 0},        {0, 0},        {0, 0},
    {0, 0},        {0, 0},        {0, 0},        {0, 0},
    {0, HID_KEY_BACKSPACE}, {0, HID_KEY_TAB}, {0, HID_KEY_ENTER}, {0, 0},
    {0, 0},        {0, HID_KEY_ENTER}, {0, 0},   {0, 0},
    {0, 0},        {0, 0},        {0, 0},        {0, 0},
    {0, 0},        {0, 0},        {0, 0},        {0, 0},
    {0, 0},        {0, 0},        {0, 0},        {0, HID_KEY_ESCAPE},
    {0, 0},        {0, 0},        {0, 0},        {0, 0},
    {0, HID_KEY_SPACE},        {1, HID_KEY_1},        {1, HID_KEY_APOSTROPHE}, {1, HID_KEY_3},
    {1, HID_KEY_4},            {1, HID_KEY_5},        {1, HID_KEY_7},          {0, HID_KEY_APOSTROPHE},
    {1, HID_KEY_9},            {1, HID_KEY_0},        {1, HID_KEY_8},          {1, HID_KEY_EQUAL},
    {0, HID_KEY_COMMA},        {0, HID_KEY_MINUS},    {0, HID_KEY_PERIOD},     {0, HID_KEY_SLASH},
    {0, HID_KEY_0},            {0, HID_KEY_1},        {0, HID_KEY_2},          {0, HID_KEY_3},
    {0, HID_KEY_4},            {0, HID_KEY_5},        {0, HID_KEY_6},          {0, HID_KEY_7},
    {0, HID_KEY_8},            {0, HID_KEY_9},        {1, HID_KEY_SEMICOLON},  {0, HID_KEY_SEMICOLON},
    {1, HID_KEY_COMMA},        {0, HID_KEY_EQUAL},    {1, HID_KEY_PERIOD},     {1, HID_KEY_SLASH},
    {1, HID_KEY_2},            {1, HID_KEY_A},        {1, HID_KEY_B},          {1, HID_KEY_C},
    {1, HID_KEY_D},            {1, HID_KEY_E},        {1, HID_KEY_F},          {1, HID_KEY_G},
    {1, HID_KEY_H},            {1, HID_KEY_I},        {1, HID_KEY_J},          {1, HID_KEY_K},
    {1, HID_KEY_L},            {1, HID_KEY_M},        {1, HID_KEY_N},          {1, HID_KEY_O},
    {1, HID_KEY_P},            {1, HID_KEY_Q},        {1, HID_KEY_R},          {1, HID_KEY_S},
    {1, HID_KEY_T},            {1, HID_KEY_U},        {1, HID_KEY_V},          {1, HID_KEY_W},
    {1, HID_KEY_X},            {1, HID_KEY_Y},        {1, HID_KEY_Z},          {0, HID_KEY_BRACKET_LEFT},
    {0, HID_KEY_BACKSLASH},    {0, HID_KEY_BRACKET_RIGHT}, {1, HID_KEY_6},     {1, HID_KEY_MINUS},
    {0, HID_KEY_GRAVE},        {0, HID_KEY_A},        {0, HID_KEY_B},          {0, HID_KEY_C},
    {0, HID_KEY_D},            {0, HID_KEY_E},        {0, HID_KEY_F},          {0, HID_KEY_G},
    {0, HID_KEY_H},            {0, HID_KEY_I},        {0, HID_KEY_J},          {0, HID_KEY_K},
    {0, HID_KEY_L},            {0, HID_KEY_M},        {0, HID_KEY_N},          {0, HID_KEY_O},
    {0, HID_KEY_P},            {0, HID_KEY_Q},        {0, HID_KEY_R},          {0, HID_KEY_S},
    {0, HID_KEY_T},            {0, HID_KEY_U},        {0, HID_KEY_V},          {0, HID_KEY_W},
    {0, HID_KEY_X},            {0, HID_KEY_Y},        {0, HID_KEY_Z},          {1, HID_KEY_BRACKET_LEFT},
    {1, HID_KEY_BACKSLASH},    {1, HID_KEY_BRACKET_RIGHT}, {1, HID_KEY_GRAVE}, {0, HID_KEY_DELETE},
};

void hid_init(void) { /* nothing beyond USB/BLE bring-up done elsewhere */ }

// "Is a USB host actively driving the bus?" — prefer USB for output only then.
// This board has no VBUS sensing, so tud_mounted() alone stays true after the
// cable is pulled (only plug-IN is detected, via enumeration). When the cable is
// pulled the host stops sending SOF, the bus goes idle, and tud_suspended() trips
// within ~3 ms — that's what lets output fall back to BLE on unplug.
static bool use_usb(void)
{
    return tud_mounted() && !tud_suspended();
}

// ── Keyboard ──────────────────────────────────────────────────────────────────

static void kbd_report(uint8_t mods, uint8_t keycode)
{
    if (use_usb()) {
        if (!tud_hid_ready()) return;
        uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};
        tud_hid_keyboard_report(HID_REPORT_ID_KEYBOARD, mods, keycode ? keys : NULL);
    } else {
        ble_hid_keyboard_report(mods, keycode);
    }
}

static void kbd_release_all(void)
{
    if (use_usb()) {
        if (tud_hid_ready()) tud_hid_keyboard_report(HID_REPORT_ID_KEYBOARD, 0, NULL);
    } else {
        ble_hid_keyboard_report(0, 0);
    }
}

void hid_tap_ascii(uint8_t ascii)
{
    if (ascii >= 128) return;
    uint8_t keycode = ascii2kc[ascii][1];
    if (keycode == 0) return;
    uint8_t mods = ascii2kc[ascii][0] ? KBD_MOD_SHIFT : 0;

    kbd_report(mods, keycode);
    vTaskDelay(pdMS_TO_TICKS(12));
    kbd_release_all();
    vTaskDelay(pdMS_TO_TICKS(8));
}

void hid_type_text(const char *text)
{
    for (const char *p = text; *p; ++p) {
        hid_tap_ascii((uint8_t)*p);
    }
}

// val 0xF0xx carries a raw HID keycode (0xxx) for keys with no ASCII form.
#define HID_HOTKEY_RAW_MASK 0xFF00
#define HID_HOTKEY_RAW_FLAG 0xF000

void hid_hotkey(uint16_t key, uint8_t mods)
{
    uint8_t keycode;
    if ((key & HID_HOTKEY_RAW_MASK) == HID_HOTKEY_RAW_FLAG) {
        keycode = (uint8_t)(key & 0xFF);          // special key (PrintScreen, F-keys…)
    } else if (key < 128) {
        keycode = ascii2kc[key][1];
        // The character's own shift requirement is folded into the explicit mods.
        if (ascii2kc[key][0]) mods |= KBD_MOD_SHIFT;
    } else {
        keycode = 0;
    }

    kbd_report(mods, keycode);
    vTaskDelay(pdMS_TO_TICKS(30));
    kbd_release_all();
    vTaskDelay(pdMS_TO_TICKS(8));
}

// ── Consumer control (media) ──────────────────────────────────────────────────

void hid_media_press(uint16_t usage)
{
    if (use_usb()) {
        if (tud_hid_ready()) tud_hid_report(HID_REPORT_ID_CONSUMER, &usage, 2);
    } else {
        ble_hid_consumer_report(usage);
    }
}

void hid_media_release(void)
{
    uint16_t zero = 0;
    if (use_usb()) {
        if (tud_hid_ready()) tud_hid_report(HID_REPORT_ID_CONSUMER, &zero, 2);
    } else {
        ble_hid_consumer_report(0);
    }
}
