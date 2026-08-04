// OpenBricx Deck — NVS-backed macro resolver (ported from MacroResolver.h).
#pragma once
#include <stdint.h>

// Action modes (match the plugin's Mode enum and the original firmware).
enum {
    MODE_NONE       = 0,
    MODE_KEYBOARD   = 1,
    MODE_MEDIA      = 2,
    MODE_HOTKEY     = 3,
    MODE_TEXT       = 4,
    MODE_LAUNCH_APP = 5,  // PC-handled
    MODE_OPEN_LINK  = 6,  // PC-handled
};

void macros_init(void);

uint8_t macros_get_page(void);
void    macros_set_page(uint8_t page);
void    macros_next_page(void);

// Resolve and fire the action bound to a button (0-based; index 9 = encoder click).
// PC-handled modes (5/6) emit `E<page>:<btn>` over the OBX serial channel instead
// of acting locally.
void macros_fire(uint8_t btn_index);

// Persist a macro slot. `prof`/`btn` are as sent by the host (btn is 1-based).
void macros_update(uint8_t prof, uint8_t btn, uint8_t mode, uint16_t val, uint8_t mods);
void macros_update_text(uint8_t prof, uint8_t btn, const char *text);

void macros_wipe(void);
