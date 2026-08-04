// OpenBricx Deck — ST7789 panel bring-up + Mochi UI renderer.
//
// Ports the original LovyanGFX "Mochi" engine onto esp_lcd + a small banded
// software renderer (gfx.c). Because this board has no PSRAM we don't keep a full
// 240x240 framebuffer; instead each frame is drawn in horizontal bands into a
// small internal DMA buffer and pushed band-by-band. The animation state is
// advanced once per frame (mochi_update) and the draw pass is pure, so it can be
// re-run per band without side effects.

#include "display.h"
#include "pinout.h"
#include "config.h"
#include "gfx.h"
#include "wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static bool s_ready = false;        // panel up
static bool s_render = false;       // band buffer allocated → renderer active

// ── Render band ────────────────────────────────────────────────────────────────
// 48 rows × 240 px × 2 B = ~23 KB, split into 5 bands per frame. Double-buffered:
// esp_lcd's draw_bitmap queues the DMA and returns *before* it completes (it only
// drains on the next call), so we must not touch a band while its transfer is in
// flight. Two buffers ping-pong — the next band is drawn into the spare while the
// current one is being sent, and the intervening draw_bitmap drains the older one.
#define BAND_H   48
#define FRAME_MS 33                 // ~30 FPS
#define UPDATE_FRAME_MS 500         // ~2 FPS while the Wi-Fi/OTA screen is up
static uint16_t *s_band[2];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static int rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ── Persisted settings (mirrors the original "mochi_settings" namespace) ───────
static const char *NVS_NS = "mochi";

static void nvs_save_int(const char *key, int val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

// ── Mochi state ────────────────────────────────────────────────────────────────
typedef enum {
    ST_IDLE, ST_SYS_HUD, ST_SYS_INFO, ST_KEYBINDS,
    ST_NOW_PLAYING, ST_PROFILE_SWITCH, ST_SETTINGS, ST_EMOTE,
} mochi_state_t;

#define MODE_MOCHI    0
#define MODE_STATS    1
#define MODE_KEYBINDS 2
#define VOL_SYNC_TIMEOUT_MS 3000

// Emote preview (cycled by a single touch tap): 0 = happy face, 1 = patted face
// (squished eyes + blush + floating hearts).
#define EMOTE_COUNT      2
#define EMOTE_MAX_HEARTS 6

static struct {
    mochi_state_t state;
    uint32_t state_timer;
    int mode;                       // user-selected idle screen

    // Volume HUD
    int vol;
    bool vol_up;
    uint32_t last_vol_sync;

    // System info
    int cpu, cpu_temp, gpu, gpu_temp, ram_pct;
    float ram_used, ram_total;

    // Now playing
    char np_title[64];
    char np_artist[48];
    bool np_playing;
    int np_scroll_x;
    uint32_t np_last_scroll;

    // Profile switch
    int profile_num;
    char profile_names[DECK_MAX_PAGES][20];

    // Settings menu
    int menu_index;
    bool in_submenu;
    int brightness;

    // Status
    int bat_pct;
    bool charging, no_battery, ble;
    uint8_t btn_modes[9];

    // Idle eye animation
    float eye_x, eye_tx, eye_y, eye_ty, eye_sy, eye_tsy;
    float eye_ls, eye_rs, eye_tls, eye_trs;   // per-eye size scale (asymmetric stare)
    uint32_t next_blink, next_gaze;
    bool blinking;

    // Emote preview (touch single-tap)
    int emote;
    struct { int16_t x, y; int8_t size, wobble; bool active; } hearts[EMOTE_MAX_HEARTS];
    uint32_t last_heart_spawn;
} M;

static uint32_t s_last_frame;

// ── Backlight (LEDC PWM) ──────────────────────────────────────────────────────
#define BL_TIMER   LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_MODE    LEDC_LOW_SPEED_MODE
#define BL_RES     LEDC_TIMER_8_BIT
#define BL_DIM_DUTY 64              // ~25% of 255 — load-shed level during OTA

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BL_MODE, .timer_num = BL_TIMER,
        .duty_resolution = BL_RES, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    ledc_channel_config_t channel = {
        // Start DARK (duty 0). The backlight is the single biggest load on the
        // 3.3 V rail (~40 mA @ 80 %); on a battery cold-start that current, stacked
        // on the flash + BLE-radio bring-up inrush, sags the AMS1117 below the
        // brownout trip and the deck never gets past boot (blank screen — yet it
        // runs fine if booted on USB and then unplugged, because the inrush is
        // already past). Keeping it off through boot pulls that load out of the
        // critical window; the first battery poll lights it (see display_set_power_source).
        .gpio_num = PIN_TFT_BLK, .speed_mode = BL_MODE, .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER, .duty = 0, .hpoint = 0,
    };
    ledc_channel_config(&channel);
}

// Push a raw 8-bit duty to the backlight channel. Does NOT touch M.brightness.
static void backlight_duty(uint32_t duty)
{
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

static uint32_t brightness_duty(int pct)
{
    uint32_t duty = (uint32_t)pct * 255 / 100;
    if (pct == 1 && duty < 3) duty = 3;   // keep it visible at the floor
    return duty;
}

void display_set_brightness(int pct)
{
    pct = clampi(pct, 1, 100);
    M.brightness = pct;
    backlight_duty(brightness_duty(pct));
}

// ── Power-source backlight level ──────────────────────────────────────────────
// Normal-operation brightness is chosen by power source: the saved level on USB,
// a fixed dim (~25%) on battery. Dimming on battery both saves runtime and keeps
// the steady-state load off the strained AMS1117 rail. main.c calls this from the
// battery poll on every source change (and once on the first poll, which is what
// actually lights the backlight after the dark boot above).
static bool s_on_battery = false;

void display_set_power_source(bool on_battery)
{
    s_on_battery = on_battery;
    backlight_duty(on_battery ? BL_DIM_DUTY : brightness_duty(M.brightness));
}

// ── Load-shed backlight control (Update / OTA) ────────────────────────────────
// While the Wi-Fi AP is up the backlight is the single biggest current draw on
// the 3.3 V rail. These poke the LEDC duty directly WITHOUT mutating M.brightness,
// so the user's saved level survives and _restore() puts it back exactly. _restore
// is power-source aware so leaving OTA on battery returns to the dim level, not full.
void display_backlight_off(void)     { backlight_duty(0); }            // RF inrush moment
void display_backlight_dim(void)     { backlight_duty(BL_DIM_DUTY); }  // through the transfer
void display_backlight_restore(void) { backlight_duty(s_on_battery ? BL_DIM_DUTY : brightness_duty(M.brightness)); }

// ── Init ──────────────────────────────────────────────────────────────────────
bool display_init(void)
{
    memset(&M, 0, sizeof(M));
    M.vol = 50;
    M.eye_x = M.eye_tx = 120; M.eye_y = M.eye_ty = 100;
    M.eye_sy = M.eye_tsy = 1.0f;
    M.eye_ls = M.eye_rs = M.eye_tls = M.eye_trs = 1.0f;
    M.brightness = 80;
    static const char *defaults[DECK_MAX_PAGES] = {"Default", "Streaming", "Productivity", "Music"};
    for (int i = 0; i < DECK_MAX_PAGES; i++)
        strncpy(M.profile_names[i], defaults[i], sizeof(M.profile_names[i]) - 1);

    backlight_init();

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_TFT_SCLK,
        .mosi_io_num = PIN_TFT_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * BAND_H * (int)sizeof(uint16_t) + 64,
    };
    if (spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return false;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_TFT_DC,
        .cs_gpio_num = -1,            // many 240x240 modules tie CS to GND
        .pclk_hz = TFT_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,               // ST7789 no-CS modules need mode 3
        .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_cfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "panel IO init failed");
        return false;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_TFT_RST,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 init failed");
        return false;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);   // matches LovyanGFX invert=true
    esp_lcd_panel_disp_on_off(s_panel, true);
    s_ready = true;

    // Render bands — internal DMA-capable memory, double-buffered (see above).
    size_t band_bytes = TFT_WIDTH * BAND_H * sizeof(uint16_t);
    s_band[0] = heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_band[1] = heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_render = (s_band[0] != NULL && s_band[1] != NULL);
    if (!s_render) ESP_LOGE(TAG, "render band alloc failed (2x%u B) — UI disabled", (unsigned)band_bytes);

    // Restore persisted brightness + idle mode.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "brt", &v) == ESP_OK) M.brightness = clampi(v, 1, 100);
        if (nvs_get_i32(h, "mode", &v) == ESP_OK) M.mode = clampi(v, 0, 2);
        for (int i = 0; i < DECK_MAX_PAGES; i++) {
            char key[8]; snprintf(key, sizeof(key), "pname%d", i);
            size_t len = sizeof(M.profile_names[i]);
            nvs_get_str(h, key, M.profile_names[i], &len);
        }
        nvs_close(h);
    }

    M.state = (M.mode == MODE_STATS) ? ST_SYS_INFO
            : (M.mode == MODE_KEYBINDS) ? ST_KEYBINDS : ST_IDLE;
    M.next_blink = now_ms() + 2000;
    M.next_gaze  = now_ms() + 1500;

    display_set_brightness(M.brightness);
    ESP_LOGI(TAG, "ST7789 ready (%dx%d), renderer %s", TFT_WIDTH, TFT_HEIGHT,
             s_render ? "on" : "off");
    return true;
}

// ── Emote: floating hearts (state advanced once per frame) ─────────────────────
static void emote_spawn_heart(void)
{
    for (int i = 0; i < EMOTE_MAX_HEARTS; i++) {
        if (!M.hearts[i].active) {
            M.hearts[i].x = 40 + rnd(0, 160);
            M.hearts[i].y = 185 + rnd(0, 45);
            M.hearts[i].size = 6 + rnd(0, 6);
            M.hearts[i].wobble = (rnd(0, 1) == 0) ? -1 : 1;
            M.hearts[i].active = true;
            return;
        }
    }
}

static void emote_update_hearts(uint32_t now)
{
    if (now - M.last_heart_spawn > 280) {
        emote_spawn_heart();
        M.last_heart_spawn = now;
    }
    for (int i = 0; i < EMOTE_MAX_HEARTS; i++) {
        if (!M.hearts[i].active) continue;
        M.hearts[i].y -= 2;                              // drift up
        M.hearts[i].x += M.hearts[i].wobble;             // gentle wobble
        if (rnd(0, 3) == 0) M.hearts[i].wobble = -M.hearts[i].wobble;
        if (M.hearts[i].y < -20) { M.hearts[i].active = false; continue; }
        if (M.hearts[i].y < 90 && M.hearts[i].size > 3 && rnd(0, 4) == 0)
            M.hearts[i].size--;                          // shrink as they rise
    }
}

// ── Animation update (mutates state; runs once per frame) ──────────────────────
static void mochi_update(uint32_t now)
{
    // State timeouts.
    switch (M.state) {
    case ST_SYS_HUD:
        if (now - M.state_timer > 1500) M.state = (M.mode == MODE_STATS) ? ST_SYS_INFO : ST_IDLE;
        break;
    case ST_NOW_PLAYING:
        if (now - M.state_timer > 5000) M.state = ST_IDLE;
        break;
    case ST_PROFILE_SWITCH:
        if (now - M.state_timer > 1500) M.state = ST_IDLE;
        break;
    default: break;
    }

    if (M.state == ST_IDLE) {
        // Random blink.
        if (now > M.next_blink) {
            if (!M.blinking) { M.blinking = true; M.eye_tsy = 0.1f; M.next_blink = now + 150; }
            else { M.blinking = false; M.eye_tsy = 1.0f; M.next_blink = now + 2000 + rnd(0, 4000); }
        }
        // Random gaze shift — and, on some of those stares, a subtle asymmetric
        // look where the eye on the side it's looking toward grows a little (the
        // near eye when it turns). Otherwise the eyes relax back to even. Only on a
        // gaze change, only sometimes, and only when looking off-center.
        if (now > M.next_gaze) {
            M.eye_tx = 120 + rnd(-30, 30);
            M.eye_ty = 100 + rnd(-15, 15);
            M.next_gaze = now + 1500 + rnd(0, 3000);

            int look = (int)M.eye_tx - 120;                  // <0 left, >0 right
            if (look != 0 && rnd(0, 2) == 0) {               // ~1 in 3 sideways stares
                float big   = 1.08f + rnd(0, 10) / 100.0f;   // 1.08 .. 1.18
                float small = 0.82f + rnd(0, 10) / 100.0f;   // 0.82 .. 0.92
                if (look < 0) { M.eye_tls = big;   M.eye_trs = small; }   // looking left  → left eye bigger
                else          { M.eye_tls = small; M.eye_trs = big;   }   // looking right → right eye bigger
            } else {
                M.eye_tls = M.eye_trs = 1.0f;                // even eyes
            }
        }
        // Smooth interpolation.
        M.eye_x  += (M.eye_tx  - M.eye_x)  * 0.15f;
        M.eye_y  += (M.eye_ty  - M.eye_y)  * 0.15f;
        M.eye_sy += (M.eye_tsy - M.eye_sy) * 0.30f;
        M.eye_ls += (M.eye_tls - M.eye_ls) * 0.18f;
        M.eye_rs += (M.eye_trs - M.eye_rs) * 0.18f;
    }

    if (M.state == ST_NOW_PLAYING) {
        int tw = gfx_text_width(M.np_title, 3);   // width only — no draw target needed
        if (tw > 230 && now - M.np_last_scroll > 30) {
            M.np_scroll_x -= 2;
            if (M.np_scroll_x < -(tw + 50)) M.np_scroll_x = 240;
            M.np_last_scroll = now;
        }
    }

    if (M.state == ST_EMOTE && M.emote == 1) {
        emote_update_hearts(now);
    }
}

static bool vol_synced(void)
{
    return M.last_vol_sync != 0 && (now_ms() - M.last_vol_sync) < VOL_SYNC_TIMEOUT_MS;
}

// ── Per-state draw (pure; called once per band) ────────────────────────────────
static void draw_eye(int x, int y, int w, int h, uint16_t color)
{
    if (h < 2) h = 2;
    gfx_fill_round_rect(x - w / 2, y - h / 2, w, h, 18, color);
}

static void draw_idle(void)
{
    uint16_t eye = GFX_CYAN;
    // Per-eye scale gives the occasional asymmetric look; eye_sy (blink) still
    // squishes both heights together.
    int wl = (int)(70 * M.eye_ls);
    int wr = (int)(70 * M.eye_rs);
    int hl = (int)(60 * M.eye_sy * M.eye_ls);
    int hr = (int)(60 * M.eye_sy * M.eye_rs);
    draw_eye((int)M.eye_x - 45, (int)M.eye_y, wl, hl, eye);
    draw_eye((int)M.eye_x + 45, (int)M.eye_y, wr, hr, eye);
}

static void draw_sys_hud(void)
{
    gfx_text(120, 50, "VOLUME", 3, GFX_WHITE, GFX_TC);
    if (vol_synced()) {
        gfx_draw_round_rect(40, 110, 160, 20, 10, GFX_DARKGREY);
        int fill_w = M.vol * 156 / 100;
        gfx_fill_round_rect(42, 112, fill_w, 16, 8, GFX_CYAN);
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", M.vol);
        gfx_text(120, 150, buf, 2, GFX_WHITE, GFX_TC);
        if (M.vol_up) gfx_fill_triangle(120, 200, 100, 220, 140, 220, GFX_GREEN);
        else          gfx_fill_triangle(120, 220, 100, 200, 140, 200, GFX_RED);
    } else {
        // No app level: show direction only.
        if (M.vol_up) {
            gfx_fill_triangle(120, 105, 75, 175, 165, 175, GFX_GREEN);
            gfx_text(120, 188, "+", 5, GFX_GREEN, GFX_TC);
        } else {
            gfx_fill_triangle(75, 105, 165, 105, 120, 175, GFX_RED);
            gfx_text(120, 188, "-", 5, GFX_RED, GFX_TC);
        }
    }
}

static void draw_bt_icon(int x, int y, uint16_t c)
{
    gfx_line(x, y, x, y + 12, c);
    gfx_line(x, y, x + 4, y + 3, c);
    gfx_line(x + 4, y + 3, x - 4, y + 9, c);
    gfx_line(x - 4, y + 3, x + 4, y + 9, c);
    gfx_line(x + 4, y + 9, x, y + 12, c);
}

static void draw_chip_icon(int x, int y, int size, uint16_t c, const char *label)
{
    int pin_len = 3, pins = 4;
    for (int i = 0; i < pins; i++) {
        int p = (size / (pins + 1)) * (i + 1);
        gfx_fill_rect(x + p - 1, y - pin_len, 2, pin_len, c);
        gfx_fill_rect(x + p - 1, y + size, 2, pin_len, c);
        gfx_fill_rect(x - pin_len, y + p - 1, pin_len, 2, c);
        gfx_fill_rect(x + size, y + p - 1, pin_len, 2, c);
    }
    gfx_draw_round_rect(x, y, size, size, 2, c);
    gfx_draw_rect(x + 2, y + 2, size - 4, size - 4, c);
    gfx_text(x + size / 2, y + size / 2, label, 1, c, GFX_MC);
}

static void draw_stat_row(int y, uint16_t icon_c, const char *label, int pct, int temp)
{
    int icon = 32;
    draw_chip_icon(20, y, icon, icon_c, label);
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", pct);
    gfx_text(65, y + 16, buf, 3, GFX_WHITE, GFX_ML);
    char tb[12]; snprintf(tb, sizeof(tb), "(%d C)", temp);
    uint16_t dim = gfx_rgb(180, 180, 200);
    gfx_text(145, y + 18, tb, 2, dim, GFX_ML);
    // degree mark over the space before "C"
    char head[8]; snprintf(head, sizeof(head), "(%d", temp);
    int off = gfx_text_width(head, 2);
    gfx_draw_circle(145 + off + 4, y + 16, 2, dim);
}

static void draw_sys_info(void)
{
    // Status bar (top right): battery + BLE.
    int bx = 230, by = 10;
    uint16_t bat_c = M.bat_pct > 20 ? GFX_GREEN : GFX_RED;
    if (M.charging) bat_c = GFX_YELLOW;
    gfx_draw_round_rect(bx - 30, by, 25, 12, 2, GFX_WHITE);
    gfx_draw_rect(bx - 5, by + 3, 2, 6, GFX_WHITE);
    if (!M.no_battery) gfx_fill_rect(bx - 28, by + 2, M.bat_pct * 21 / 100, 8, bat_c);
    else gfx_text(bx - 18, by + 6, "USB", 1, GFX_WHITE, GFX_MC);
    if (M.ble) draw_bt_icon(bx - 45, by, GFX_CYAN);

    gfx_text(120, 30, "SYSTEM MONITOR", 2, GFX_WHITE, GFX_TC);
    gfx_hline(30, 52, 180, gfx_rgb(60, 60, 70));

    draw_stat_row(70,  GFX_CYAN,    "CPU", M.cpu, M.cpu_temp);
    draw_stat_row(120, GFX_MAGENTA, "GPU", M.gpu, M.gpu_temp);

    // RAM row (used/total, no temp).
    int y = 170, icon = 32;
    draw_chip_icon(20, y, icon, GFX_GREEN, "RAM");
    char ram[24]; snprintf(ram, sizeof(ram), "%.1f/%.1f GB", M.ram_used, M.ram_total);
    gfx_text(65, y + 16, ram, 2, GFX_WHITE, GFX_ML);
}

static void draw_keybinds(void)
{
    int cell = 76, pad = 4;
    static const char *icon[7] = {"", "K", "M", "H", "T", "A", "L"};
    static const uint16_t col[7] = {
        0, GFX_WHITE, GFX_CYAN, GFX_YELLOW, GFX_GREEN, GFX_MAGENTA, GFX_BLUE,
    };
    for (int i = 0; i < 9; i++) {
        int x = (i % 3) * (cell + pad) + 2;
        int y = (i / 3) * (cell + pad) + 2;
        gfx_draw_round_rect(x, y, cell, cell, 8, gfx_rgb(50, 50, 70));
        gfx_fill_round_rect(x + 2, y + 2, cell - 4, cell - 4, 6, gfx_rgb(25, 25, 35));
        int m = M.btn_modes[i];
        int cx = x + cell / 2, cy = y + cell / 2;
        if (m >= 1 && m <= 6) gfx_text(cx, cy, icon[m], 3, col[m], GFX_MC);
        else gfx_fill_circle(cx, cy, 3, gfx_rgb(60, 60, 70));
    }
}

static void draw_now_playing(void)
{
    gfx_text(120, 30, "PLAYING", 3, GFX_MAGENTA, GFX_TC);

    int tw = gfx_text_width(M.np_title, 3);
    if (tw > 230) gfx_text(M.np_scroll_x, 110, M.np_title, 3, GFX_WHITE, GFX_ML);
    else gfx_text(120, 110, M.np_title, 3, GFX_WHITE, GFX_MC);

    char artist[sizeof(M.np_artist) + 4];
    if (strlen(M.np_artist) > 18) snprintf(artist, sizeof(artist), "%.16s..", M.np_artist);
    else snprintf(artist, sizeof(artist), "%s", M.np_artist);
    gfx_text(120, 150, artist, 2, gfx_rgb(180, 180, 200), GFX_MC);

    gfx_fill_round_rect(60, 190, 120, 4, 2, GFX_MAGENTA);
}

static void draw_profile_switch(void)
{
    gfx_text(120, 60, "PROFILE", 2, GFX_WHITE, GFX_TC);
    char n[12]; snprintf(n, sizeof(n), "%d", M.profile_num + 1);
    gfx_text(120, 100, n, 5, GFX_CYAN, GFX_TC);
    if (M.profile_num < DECK_MAX_PAGES)
        gfx_text(120, 170, M.profile_names[M.profile_num], 2, gfx_rgb(180, 180, 200), GFX_TC);
}

// Drawn while inside the "Update" item: the AP is live, show how to reach it.
static void draw_update_screen(void)
{
    gfx_text(120, 18, "SETTINGS", 2, GFX_WHITE, GFX_TC);
    gfx_hline(40, 46, 160, GFX_DARKGREY);

    gfx_text(120, 64, "UPDATE MODE", 2, GFX_GREEN, GFX_MC);
    gfx_text(120, 92, "Join Wi-Fi:", 1, GFX_DARKGREY, GFX_MC);
    gfx_text(120, 108, wifi_ap_ssid(), 2, GFX_CYAN, GFX_MC);

    char pass[40];
    snprintf(pass, sizeof(pass), "pass: %s", wifi_ap_pass());
    gfx_text(120, 134, pass, 1, GFX_WHITE, GFX_MC);

    char url[40];
    snprintf(url, sizeof(url), "http://%s", wifi_ap_ip());
    gfx_text(120, 158, url, 2, GFX_WHITE, GFX_MC);

    int n = wifi_ap_clients();
    char cl[24];
    snprintf(cl, sizeof(cl), "%d connected", n);
    gfx_text(120, 188, cl, 1, n > 0 ? GFX_GREEN : GFX_DARKGREY, GFX_MC);

    // AP bring-up diagnostic: "AP OK tx=34" means the radio started; "FAIL …"
    // names the step that broke. If it says OK but no SSID appears, it's RF/power.
    const char *st = wifi_ap_status();
    bool ok = st[0] == 'A';  // "AP OK …"
    gfx_text(120, 210, st, 1, ok ? GFX_DARKGREY : GFX_RED, GFX_MC);
}

// Brightness adjust (item 0 submenu) — its own screen with the slider.
static void draw_brightness_screen(void)
{
    gfx_text(120, 20, "SETTINGS", 2, GFX_WHITE, GFX_TC);
    gfx_hline(40, 50, 160, GFX_DARKGREY);
    gfx_text(120, 95, "Brightness", 2, GFX_CYAN, GFX_MC);
    gfx_draw_round_rect(40, 135, 160, 20, 10, GFX_DARKGREY);
    int fill_w = M.brightness * 156 / 100;
    gfx_fill_round_rect(42, 137, fill_w, 16, 8, GFX_CYAN);
    char b[8]; snprintf(b, sizeof(b), "%d%%", M.brightness);
    gfx_text(120, 178, b, 2, GFX_WHITE, GFX_MC);
}

// Drawn while inside the "Pairing" item: BLE is discoverable, walk the user through
// adding the deck on their phone/PC. Flips to a "connected" confirmation once a host
// attaches (M.ble is fed by display_set_ble_connected from the main loop).
static void draw_pairing_screen(void)
{
    gfx_text(120, 18, "SETTINGS", 2, GFX_WHITE, GFX_TC);
    gfx_hline(40, 46, 160, GFX_DARKGREY);

    if (M.ble) {
        gfx_text(120, 96, "CONNECTED", 3, GFX_GREEN, GFX_MC);
        gfx_text(120, 150, "Bluetooth host paired", 1, GFX_DARKGREY, GFX_MC);
        return;
    }

    gfx_text(120, 70, "PAIRING MODE", 2, GFX_CYAN, GFX_MC);
    gfx_text(120, 104, "On your phone or PC,", 1, GFX_DARKGREY, GFX_MC);
    gfx_text(120, 122, "open Bluetooth and add:", 1, GFX_DARKGREY, GFX_MC);
    gfx_text(120, 158, OBX_DEVICE_NAME, 2, GFX_WHITE, GFX_MC);
    gfx_text(120, 200, "Leave to stop pairing", 1, GFX_DARKGREY, GFX_MC);
}

static void draw_settings(void)
{
    // Each item opens onto its own full-screen submenu.
    if (M.in_submenu) {
        switch (M.menu_index) {
        case 0: draw_brightness_screen(); return;
        case 1: draw_pairing_screen();    return;
        case 2: draw_update_screen();     return;
        default: return;
        }
    }

    gfx_text(120, 20, "SETTINGS", 2, GFX_WHITE, GFX_TC);
    gfx_hline(40, 50, 160, GFX_DARKGREY);

    static const char *labels[3] = { "Brightness", "Pairing", "Update" };
    for (int i = 0; i < 3; i++) {
        int top = 70 + i * 42;   // 70 / 112 / 154
        if (M.menu_index == i)
            gfx_fill_round_rect(30, top, 180, 30, 6, gfx_rgb(40, 40, 60));
        gfx_text(120, top + 15, labels[i], 2, M.menu_index == i ? GFX_CYAN : GFX_WHITE, GFX_MC);
    }
}

// ── Emote preview (ported from the ST7735 "cute face" sketch, scaled to 240) ───
static void draw_emote_smile(void)
{
    // Minimalist upturned smile, a few rows thick for visibility.
    for (int i = 0; i < 3; i++) {
        gfx_line(100, 158 + i, 120, 166 + i, GFX_CYAN);
        gfx_line(120, 166 + i, 140, 158 + i, GFX_CYAN);
    }
}

static void draw_emote_happy(void)
{
    gfx_fill_round_rect(64, 70, 40, 56, 14, GFX_CYAN);    // left eye
    gfx_fill_round_rect(136, 70, 40, 56, 14, GFX_CYAN);   // right eye
    draw_emote_smile();
}

static void draw_heart(int cx, int cy, int s, uint16_t color)
{
    int r = s;
    gfx_fill_circle(cx - r + 1, cy - r / 2, r, color);
    gfx_fill_circle(cx + r - 1, cy - r / 2, r, color);
    gfx_fill_triangle(cx - 2 * r + 1, cy, cx + 2 * r - 1, cy, cx, cy + 2 * r, color);
}

static void draw_emote_patted(void)
{
    // Squished-happy eyes
    gfx_fill_round_rect(64, 96, 40, 16, 8, GFX_CYAN);
    gfx_fill_round_rect(136, 96, 40, 16, 8, GFX_CYAN);
    // Pink blush under each eye
    uint16_t blush = gfx_rgb(255, 130, 150);
    gfx_fill_circle(80, 128, 12, blush);
    gfx_fill_circle(160, 128, 12, blush);
    draw_emote_smile();
    // Floating hearts (positions advanced in emote_update_hearts)
    static const uint8_t hc[6][3] = {
        {255, 80, 120}, {255, 105, 140}, {255, 60, 80},
        {255, 150, 180}, {230, 50, 100}, {255, 120, 160},
    };
    for (int i = 0; i < EMOTE_MAX_HEARTS; i++) {
        if (!M.hearts[i].active) continue;
        uint16_t c = gfx_rgb(hc[i % 6][0], hc[i % 6][1], hc[i % 6][2]);
        draw_heart(M.hearts[i].x, M.hearts[i].y, M.hearts[i].size, c);
    }
}

static void draw_emote(void)
{
    if (M.emote == 1) draw_emote_patted();
    else              draw_emote_happy();
}

// One background for every scene. A lifted near-black rather than pure 0x0000:
// this is a backlit LCD, so true black saves no power and only makes the panel's
// backlight bleed / edge glow obvious.
static uint16_t scene_bg(void)
{
    return gfx_rgb(10, 10, 15);
}

static void render_scene(void)
{
    switch (M.state) {
    case ST_IDLE:           draw_idle(); break;
    case ST_SYS_HUD:        draw_sys_hud(); break;
    case ST_SYS_INFO:       draw_sys_info(); break;
    case ST_KEYBINDS:       draw_keybinds(); break;
    case ST_NOW_PLAYING:    draw_now_playing(); break;
    case ST_PROFILE_SWITCH: draw_profile_switch(); break;
    case ST_SETTINGS:       draw_settings(); break;
    case ST_EMOTE:          draw_emote(); break;
    }
}

// ── Frame push ─────────────────────────────────────────────────────────────────
void display_tick(void)
{
    if (!s_ready || !s_render) return;

    // Slow to ~2 FPS while the Update (Wi-Fi/OTA) screen is up: it barely changes,
    // and idling the render task frees rail current for the radio on battery.
    uint32_t frame_ms = display_update_mode() ? UPDATE_FRAME_MS : FRAME_MS;

    uint32_t now = now_ms();
    if (now - s_last_frame < frame_ms) return;
    s_last_frame = now;

    mochi_update(now);

    uint16_t bg = scene_bg();
    int idx = 0;
    for (int by = 0; by < TFT_HEIGHT; by += BAND_H) {
        int bh = (by + BAND_H <= TFT_HEIGHT) ? BAND_H : (TFT_HEIGHT - by);
        uint16_t *buf = s_band[idx];
        idx ^= 1;                       // next band draws into the spare buffer
        gfx_set_target(buf, TFT_WIDTH, by, bh);
        gfx_clear(bg);
        render_scene();
        // esp_lcd wants big-endian RGB565 (LovyanGFX used setSwapBytes(true)).
        int n = TFT_WIDTH * bh;
        for (int i = 0; i < n; i++) buf[i] = __builtin_bswap16(buf[i]);
        esp_lcd_panel_draw_bitmap(s_panel, 0, by, TFT_WIDTH, by + bh, buf);
    }
}

// ── Render task ────────────────────────────────────────────────────────────────
// Runs on its own core so the per-band DMA blocking never stalls the input/USB
// loop in deck_task. display_tick() self-gates to ~30 FPS.
static void display_task(void *arg)
{
    (void)arg;
    while (1) {
        display_tick();
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void display_start(void)
{
    if (!s_render) return;
    xTaskCreatePinnedToCore(display_task, "disp", 4096, NULL, 4, NULL, 1);
}

// ── Semantic UI calls ──────────────────────────────────────────────────────────
void display_sync_volume(int vol)
{
    vol = clampi(vol, 0, 100);
    M.last_vol_sync = now_ms();
    if (M.state == ST_SETTINGS) { M.vol = vol; return; }
    if (vol != M.vol) {
        M.vol_up = (vol > M.vol);
        M.vol = vol;
        M.state = ST_SYS_HUD;
        M.state_timer = now_ms();
    } else {
        M.vol = vol;
    }
}

void display_show_volume(bool up)
{
    if (M.state == ST_SETTINGS) return;
    M.vol_up = up;
    M.vol = up ? clampi(M.vol + 2, 0, 100) : clampi(M.vol - 2, 0, 100);
    M.state = ST_SYS_HUD;
    M.state_timer = now_ms();
}

void display_set_profile_name(int idx, const char *name)
{
    if (idx < 0 || idx >= DECK_MAX_PAGES) return;
    if (strncmp(M.profile_names[idx], name, sizeof(M.profile_names[idx])) == 0) return;
    strncpy(M.profile_names[idx], name, sizeof(M.profile_names[idx]) - 1);
    M.profile_names[idx][sizeof(M.profile_names[idx]) - 1] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        char key[8]; snprintf(key, sizeof(key), "pname%d", idx);
        nvs_set_str(h, key, M.profile_names[idx]);
        nvs_commit(h);
        nvs_close(h);
    }
}

void display_show_profile_switch(int page)
{
    M.profile_num = page;
    M.state = ST_PROFILE_SWITCH;
    M.state_timer = now_ms();
}

void display_sync_sysinfo(int cpu, int cpu_temp, float ram_used, float ram_total, int gpu, int gpu_temp)
{
    M.cpu = cpu; M.cpu_temp = cpu_temp;
    M.ram_used = ram_used; M.ram_total = ram_total;
    M.ram_pct = (ram_total > 0) ? (int)((ram_used / ram_total) * 100) : 0;
    M.gpu = gpu; M.gpu_temp = gpu_temp;
    if (M.state != ST_SETTINGS && M.mode == MODE_STATS) M.state = ST_SYS_INFO;
}

void display_show_now_playing(const char *title, const char *artist, long pos_ms, long dur_ms, bool playing)
{
    (void)pos_ms; (void)dur_ms;
    if (strncmp(M.np_title, title, sizeof(M.np_title)) != 0) {
        strncpy(M.np_title, title, sizeof(M.np_title) - 1);
        M.np_title[sizeof(M.np_title) - 1] = '\0';
        M.np_scroll_x = 240;
    }
    strncpy(M.np_artist, artist, sizeof(M.np_artist) - 1);
    M.np_artist[sizeof(M.np_artist) - 1] = '\0';
    M.np_playing = playing;
    if (M.state == ST_SETTINGS) return;
    M.state = ST_NOW_PLAYING;
    M.state_timer = now_ms();
}

void display_set_mode(int mode)
{
    M.mode = clampi(mode, 0, 2);
    nvs_save_int("mode", M.mode);
    if (M.state == ST_SETTINGS) return;
    M.state = (M.mode == MODE_STATS) ? ST_SYS_INFO
            : (M.mode == MODE_KEYBINDS) ? ST_KEYBINDS : ST_IDLE;
}

void display_cycle_mode(void)
{
    if (M.state == ST_SETTINGS) return;
    display_set_mode(M.mode == MODE_MOCHI ? MODE_STATS : MODE_MOCHI);
}

// Single touch tap: step through the emote previews. Enters the emote view on the
// first tap, advances each tap, and returns to the normal idle screen after the
// last one.
void display_emote_next(void)
{
    if (M.state == ST_SETTINGS) return;        // don't hijack the settings menu

    if (M.state != ST_EMOTE) {
        M.state = ST_EMOTE;
        M.emote = 0;
    } else if (++M.emote >= EMOTE_COUNT) {
        M.emote = 0;
        M.state = (M.mode == MODE_STATS) ? ST_SYS_INFO
                : (M.mode == MODE_KEYBINDS) ? ST_KEYBINDS : ST_IDLE;
    }

    if (M.state == ST_EMOTE && M.emote == 1) {  // (re)entering the patted emote
        for (int i = 0; i < EMOTE_MAX_HEARTS; i++) M.hearts[i].active = false;
        M.last_heart_spawn = now_ms();
    }
}

void display_update_battery(int percent, bool charging, bool no_battery)
{
    M.bat_pct = percent;
    M.charging = charging;
    M.no_battery = no_battery;
}

void display_set_ble_connected(bool connected) { M.ble = connected; }

void display_set_happy(void) { /* pet reaction — intentionally minimal */ }

// ── Settings menu (encoder) ────────────────────────────────────────────────────
void display_toggle_settings(void)
{
    if (M.state == ST_SETTINGS) {
        M.state = (M.mode == MODE_STATS) ? ST_SYS_INFO : ST_IDLE;
    } else {
        M.state = ST_SETTINGS;
        M.menu_index = 0;
        M.in_submenu = false;
    }
}

bool display_settings_open(void) { return M.state == ST_SETTINGS; }

// True while the "Update" item is open — the cue for main.c to bring up the AP.
bool display_update_mode(void)
{
    return M.state == ST_SETTINGS && M.menu_index == 2 && M.in_submenu;
}

// True while the "Pairing" item is open — the cue for main.c to enter BLE pairing.
bool display_pairing_mode(void)
{
    return M.state == ST_SETTINGS && M.menu_index == 1 && M.in_submenu;
}

#define MENU_ITEM_COUNT 3   // 0 = Brightness, 1 = Pairing, 2 = Update

void display_menu_scroll(int dir)
{
    if (M.state != ST_SETTINGS) return;
    if (!M.in_submenu) {
        // Move between menu items (wraps).
        M.menu_index = (M.menu_index + (dir > 0 ? 1 : MENU_ITEM_COUNT - 1)) % MENU_ITEM_COUNT;
    } else if (M.menu_index == 0) {
        // Brightness adjust.
        if (dir > 0) M.brightness = (M.brightness == 1) ? 5 : M.brightness + 5;
        else         M.brightness = (M.brightness <= 5) ? 1 : M.brightness - 5;
        M.brightness = clampi(M.brightness, 1, 100);
        display_set_brightness(M.brightness);
        nvs_save_int("brt", M.brightness);
    }
    // Pairing / Update submenus have nothing to scroll.
}

void display_menu_action(void)
{
    if (M.state != ST_SETTINGS) return;
    M.in_submenu = !M.in_submenu;   // enter/leave the highlighted item
}
