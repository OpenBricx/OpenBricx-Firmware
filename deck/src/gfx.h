// OpenBricx Deck — tiny software 2D renderer (band-based, no PSRAM required).
//
// The ST7789 is driven through esp_lcd's draw_bitmap, which only blits raw
// pixels — there are no shapes or text. The original firmware used LovyanGFX's
// full-screen sprite; this board has no PSRAM, so instead of one 112 KB
// framebuffer we render the screen in horizontal bands into a small internal
// buffer and push each band as it's finished.
//
// All primitives draw into the "current target" band set with gfx_set_target().
// Coordinates are in full-screen (0..239) space; pixels outside the active band
// are clipped away, so a draw call can be issued once per band and the right
// slice lands each time. Colours are native RGB565 (the caller byte-swaps for
// the panel at flush time).

#pragma once
#include <stdint.h>

// ── Named colours (RGB565) ────────────────────────────────────────────────────
#define GFX_BLACK    0x0000
#define GFX_WHITE    0xFFFF
#define GFX_CYAN     0x07FF
#define GFX_RED      0xF800
#define GFX_GREEN    0x07E0
#define GFX_BLUE     0x001F
#define GFX_YELLOW   0xFFE0
#define GFX_MAGENTA  0xF81F
#define GFX_DARKGREY 0x7BEF

static inline uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Text anchor point for gfx_text().
typedef enum {
    GFX_TL,  // top-left      (x,y is the top-left of the string)
    GFX_TC,  // top-center
    GFX_ML,  // middle-left
    GFX_MC,  // middle-center
} gfx_datum_t;

// Point the renderer at a band: `buf` is width*h pixels, representing screen rows
// [y0 .. y0+h). All later draw calls clip to this slice.
void gfx_set_target(uint16_t *buf, int width, int y0, int h);

void gfx_clear(uint16_t color);
void gfx_pixel(int x, int y, uint16_t color);
void gfx_hline(int x, int y, int w, uint16_t color);
void gfx_vline(int x, int y, int h, uint16_t color);
void gfx_line(int x0, int y0, int x1, int y1, uint16_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint16_t color);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);
void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint16_t color);
void gfx_fill_circle(int cx, int cy, int r, uint16_t color);
void gfx_draw_circle(int cx, int cy, int r, uint16_t color);
void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);

// Draw `str` using the built-in 5x7 font scaled by `size`, anchored per `datum`.
void gfx_text(int x, int y, const char *str, int size, uint16_t color, gfx_datum_t datum);

// Pixel width of `str` at the given size (matches gfx_text advance).
int gfx_text_width(const char *str, int size);
