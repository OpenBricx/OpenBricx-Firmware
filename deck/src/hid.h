// OpenBricx Deck — HID output abstraction (USB HID, BLE HID fallback).
//
// Higher layers call these semantic functions; routing to USB vs BLE happens
// inside based on whether a USB host is currently mounted.
#pragma once
#include <stdint.h>
#include <stdbool.h>

void hid_init(void);

// Tap a single ASCII character (press + release). Applies the shift level the
// character needs (e.g. '!' -> Shift+1).
void hid_tap_ascii(uint8_t ascii);

// Type a whole string, character by character.
void hid_type_text(const char *text);

// Modifier combo: key + modifier bitmask (bit0 Ctrl, 1 Shift, 2 Alt, 3 Gui).
// `key` is either an ASCII char (0–127), resolved via the ASCII→keycode map, or a
// special HID key with no ASCII equivalent encoded as 0xF000 | keycode — e.g.
// PrintScreen (0x46) is 0xF046, so Win+PrintScreen is hid_hotkey(0xF046, 0x08).
void hid_hotkey(uint16_t key, uint8_t mods);

// Consumer control (media) usage code, e.g. 0xCD play/pause, 0xE9 vol+.
void hid_media_press(uint16_t usage);
void hid_media_release(void);
