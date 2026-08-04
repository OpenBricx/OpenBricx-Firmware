// OpenBricx Deck — software 2D renderer. See gfx.h.

#include "gfx.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// ── Current band target ────────────────────────────────────────────────────────
static uint16_t *g_buf;
static int g_w;     // band width (screen width)
static int g_y0;    // screen y of band's first row
static int g_h;     // band height

void gfx_set_target(uint16_t *buf, int width, int y0, int h)
{
    g_buf = buf;
    g_w = width;
    g_y0 = y0;
    g_h = h;
}

// Plot one pixel in screen space; clipped to the band and width.
void gfx_pixel(int x, int y, uint16_t color)
{
    int py = y - g_y0;
    if ((unsigned)x >= (unsigned)g_w || (unsigned)py >= (unsigned)g_h) return;
    g_buf[py * g_w + x] = color;
}

// Fill a run of pixels on one screen row [x0 .. x1] (inclusive).
static void hspan(int x0, int x1, int y, uint16_t color)
{
    int py = y - g_y0;
    if ((unsigned)py >= (unsigned)g_h) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 > g_w - 1) x1 = g_w - 1;
    if (x1 < x0) return;
    uint16_t *row = &g_buf[py * g_w];
    for (int x = x0; x <= x1; x++) row[x] = color;
}

void gfx_clear(uint16_t color)
{
    int n = g_w * g_h;
    for (int i = 0; i < n; i++) g_buf[i] = color;
}

void gfx_hline(int x, int y, int w, uint16_t color)
{
    if (w <= 0) return;
    hspan(x, x + w - 1, y, color);
}

void gfx_vline(int x, int y, int h, uint16_t color)
{
    for (int j = 0; j < h; j++) gfx_pixel(x, y + j, color);
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int j = 0; j < h; j++) hspan(x, x + w - 1, y + j, color);
}

void gfx_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

void gfx_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_fill_circle(int cx, int cy, int r, uint16_t color)
{
    if (r < 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        hspan(cx - dx, cx + dx, cy + dy, color);
    }
}

// Adafruit-style quadrant plotter. `corners` bit0=TR bit1=TL bit2=BL bit3=BR.
static void circle_helper(int cx, int cy, int r, uint8_t corners, uint16_t color)
{
    int f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        if (corners & 0x1) { gfx_pixel(cx + x, cy - y, color); gfx_pixel(cx + y, cy - x, color); }
        if (corners & 0x2) { gfx_pixel(cx - y, cy - x, color); gfx_pixel(cx - x, cy - y, color); }
        if (corners & 0x4) { gfx_pixel(cx - y, cy + x, color); gfx_pixel(cx - x, cy + y, color); }
        if (corners & 0x8) { gfx_pixel(cx + x, cy + y, color); gfx_pixel(cx + y, cy + x, color); }
    }
}

void gfx_draw_circle(int cx, int cy, int r, uint16_t color)
{
    if (r < 0) return;
    gfx_pixel(cx, cy - r, color);
    gfx_pixel(cx, cy + r, color);
    gfx_pixel(cx - r, cy, color);
    gfx_pixel(cx + r, cy, color);
    circle_helper(cx, cy, r, 0xF, color);
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1) { gfx_fill_rect(x, y, w, h, color); return; }

    for (int j = 0; j < h; j++) {
        int from_top = j, from_bot = h - 1 - j;
        int d = from_top < from_bot ? from_top : from_bot;
        int inset = 0;
        if (d < r) {
            int k = r - 1 - d;                                   // depth into the corner
            inset = r - (int)(sqrtf((float)(r * r - k * k)) + 0.5f);
        }
        hspan(x + inset, x + w - 1 - inset, y + j, color);
    }
}

void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    gfx_hline(x + r, y, w - 2 * r, color);
    gfx_hline(x + r, y + h - 1, w - 2 * r, color);
    gfx_vline(x, y + r, h - 2 * r, color);
    gfx_vline(x + w - 1, y + r, h - 2 * r, color);
    circle_helper(x + r,         y + r,         r, 0x2, color);  // TL
    circle_helper(x + w - r - 1, y + r,         r, 0x1, color);  // TR
    circle_helper(x + w - r - 1, y + h - r - 1, r, 0x8, color);  // BR
    circle_helper(x + r,         y + h - r - 1, r, 0x4, color);  // BL
}

void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    // Sort vertices by y ascending.
    if (y0 > y1) { int t; t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 > y2) { int t; t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y0 > y1) { int t; t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }

    if (y0 == y2) {  // degenerate: a horizontal line
        int a = x0, b = x0;
        if (x1 < a) a = x1; else if (x1 > b) b = x1;
        if (x2 < a) a = x2; else if (x2 > b) b = x2;
        hspan(a, b, y0, color);
        return;
    }

    int dx01 = x1 - x0, dy01 = y1 - y0;
    int dx02 = x2 - x0, dy02 = y2 - y0;
    int dx12 = x2 - x1, dy12 = y2 - y1;
    long sa = 0, sb = 0;

    int last = (y1 == y2) ? y1 : y1 - 1;   // include the flat edge on the right half
    int y;
    for (y = y0; y <= last; y++) {
        int a = x0 + (int)(sa / (dy01 ? dy01 : 1));
        int b = x0 + (int)(sb / dy02);
        sa += dx01; sb += dx02;
        hspan(a, b, y, color);
    }
    sa = (long)dx12 * (y - y1);
    sb = (long)dx02 * (y - y0);
    for (; y <= y2; y++) {
        int a = x1 + (int)(sa / (dy12 ? dy12 : 1));
        int b = x0 + (int)(sb / dy02);
        sa += dx12; sb += dx02;
        hspan(a, b, y, color);
    }
}

// ── 5x7 font (classic GLCD glyphs, printable ASCII 0x20..0x7F) ─────────────────
// Each glyph is 5 vertical columns; bit0 = top row, bit6 = bottom row.
static const uint8_t FONT5X7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14}, {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x08,0x04,0x08,0x10,0x08}, {0x00,0x00,0x00,0x00,0x00},
};

#define GLYPH_W 5
#define GLYPH_H 7
#define ADVANCE 6   // glyph + 1px inter-char gap

int gfx_text_width(const char *str, int size)
{
    int n = (int)strlen(str);
    if (n == 0) return 0;
    return (n * ADVANCE - 1) * size;   // drop the trailing gap
}

void gfx_text(int x, int y, const char *str, int size, uint16_t color, gfx_datum_t datum)
{
    if (size < 1) size = 1;
    int w = gfx_text_width(str, size);
    int h = GLYPH_H * size;

    switch (datum) {
    case GFX_TL: break;
    case GFX_TC: x -= w / 2; break;
    case GFX_ML: y -= h / 2; break;
    case GFX_MC: x -= w / 2; y -= h / 2; break;
    }

    for (const char *p = str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        const uint8_t *glyph = (c >= 0x20 && c < 0x80) ? FONT5X7[c - 0x20] : FONT5X7[0];
        for (int col = 0; col < GLYPH_W; col++) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < GLYPH_H; row++) {
                if (bits & (1 << row)) {
                    if (size == 1) gfx_pixel(x + col, y + row, color);
                    else gfx_fill_rect(x + col * size, y + row * size, size, size, color);
                }
            }
        }
        x += ADVANCE * size;
    }
}
