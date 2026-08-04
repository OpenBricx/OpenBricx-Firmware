// OpenBricx Deck — ST7789 display + Mochi UI surface.
//
// The panel and backlight are brought up over SPI/LEDC, and the animated "Mochi"
// renderer (idle eyes, volume HUD, system monitor, now-playing, profile switch,
// settings menu) is ported from the original LovyanGFX firmware onto a small
// banded software renderer (see gfx.c) — no PSRAM framebuffer required.
//
// display_tick() must be called every loop; it self-gates to ~30 FPS. The
// semantic calls below capture state and pick the screen to show; input gestures
// (encoder/touch in main.c) drive volume/settings/mode.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Brings up the ST7789 over SPI + allocates the render band. Returns false if any
// stage (SPI bus, panel IO, ST7789 init) fails — see the boot diagnostics emitted
// over the OBX channel. If the panel is up but the render band can't be allocated,
// it still returns true and shows a static fill.
bool display_init(void);

// Per-frame hook. Advances the animation and pushes a frame (~30 FPS gate).
void display_tick(void);

// Spawn the render task (own core, so DMA blocking doesn't stall input). No-op if
// the render buffers couldn't be allocated. Call once after display_init().
void display_start(void);

// ── Semantic UI calls (driven by the OBX protocol + input) ────────────────────
void display_sync_volume(int vol);        // real system level from the companion app
void display_show_volume(bool up);         // local encoder turn (no app level known)
void display_set_profile_name(int idx, const char *name);
void display_show_profile_switch(int page);
void display_sync_sysinfo(int cpu, int cpu_temp, float ram_used, float ram_total, int gpu, int gpu_temp);
void display_show_now_playing(const char *title, const char *artist, long pos_ms, long dur_ms, bool playing);
void display_set_mode(int mode);           // 0=Mochi 1=Stats 2=Keybinds
void display_cycle_mode(void);             // touch double-tap: Mochi <-> Stats
void display_emote_next(void);             // touch single-tap: cycle emote preview
void display_set_brightness(int pct);

// Backlight level by power source: saved brightness on USB, fixed ~25% dim on
// battery. Called from the battery poll on each source change; the first call also
// lights the backlight after its dark (duty-0) boot. See display.c.
void display_set_power_source(bool on_battery);

// ── Load-shed backlight control (used by main.c around Update/OTA) ─────────────
void display_backlight_off(void);      // kill the backlight (Wi-Fi bring-up inrush)
void display_backlight_dim(void);      // low level through the OTA transfer
void display_backlight_restore(void);  // back to the power-appropriate level on exit

void display_update_battery(int percent, bool charging, bool no_battery);
void display_set_ble_connected(bool connected);
void display_set_happy(void);

// ── On-device settings menu (encoder) ─────────────────────────────────────────
void display_toggle_settings(void);        // encoder double-click
bool display_settings_open(void);
void display_menu_scroll(int dir);         // encoder rotation while settings open
void display_menu_action(void);            // encoder single-click while settings open
bool display_update_mode(void);            // true while the "Update" item is open (AP cue)
bool display_pairing_mode(void);           // true while the "Pairing" item is open (BLE cue)
