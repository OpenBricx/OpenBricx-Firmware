// OpenBricx Deck V1 — master pin mapping (ESP32-S3 SuperMini).
// Mirrors the original Arduino Pinout.h / DisplayConfig.h.
#pragma once

// ── I2C (PCF8574 I/O expander) ────────────────────────────────────────────────
// NOTE: SDA/SCL swapped vs. the original (was SDA=5, SCL=6) to test a possible
// reversed wiring — the PCF8574 wasn't ACKing on the original mapping.
#define PIN_I2C_SDA        6
#define PIN_I2C_SCL        5
#define PCF8574_ADDR       0x20

// PCF8574 pin assignments:
//   P0 -> TTP223 touch (input)         P4 -> matrix row 1 (output)
//   P1 -> matrix col 1 (input pullup)  P5 -> matrix row 2 (output)
//   P2 -> matrix col 2 (input pullup)  P6 -> matrix row 3 (output)
//   P3 -> matrix col 3 (input pullup)  P7 -> encoder switch (input pullup)
#define PCF_TOUCH          0
#define PCF_COL1           1
#define PCF_COL2           2
#define PCF_COL3           3
#define PCF_ROW1           4
#define PCF_ROW2           5
#define PCF_ROW3           6
#define PCF_ENC_SW         7

// ── Rotary encoder (HW-040) ───────────────────────────────────────────────────
#define PIN_ENC_CLK        1   // phase A
#define PIN_ENC_DT         2   // phase B

// ── ST7789 display (240x240, SPI3 / FSPI, no CS) ──────────────────────────────
#define PIN_TFT_BLK        9   // backlight PWM
#define PIN_TFT_DC         10
#define PIN_TFT_RST        11
#define PIN_TFT_MOSI       12
#define PIN_TFT_SCLK       13
#define TFT_WIDTH          240
#define TFT_HEIGHT         240
#define TFT_SPI_HZ         (40 * 1000 * 1000)   // 40 MHz ceiling: this panel is on SPI3 (pins 12/13) which has NO IOMUX on the ESP32-S3 and always routes via the GPIO matrix (~40 MHz max). 80 MHz corrupted the ST7789 command stream and froze the panel while the MCU kept running. Do NOT raise without moving to SPI2 IOMUX pins.

// ── Battery sense ─────────────────────────────────────────────────────────────
#define PIN_BAT_ADC        4   // ADC1 channel, 100k/100k divider
// Calibrated raw-count range mapped linearly to 0–100 %.
//
// MIN is the *practical* empty for THIS hardware, NOT the cell's 3.0 V floor.
// The AMS1117-3.3 LDO has ~1.1 V dropout, so the 3.3 V rail browns out while the
// cell is still ~3.7 V under load (see deck-battery-power-constraint). The deck
// therefore dies long before a 3.0 V cell — counting capacity below the brownout
// point is fiction. The old MIN=1736 (~3.0 V cell) is why the gauge read 62 %
// and then cut out on unplug. MIN was raised to the measured shutdown raw count
// so 0 % ≈ "about to brown out." If the deck still reads >0 % at shutdown, nudge
// MIN up toward MAX (watch the raw_avg log from battery_read over serial).
#define BAT_RAW_MIN        2200   // ≈ brownout floor (was 1736 / ~3.0 V cell)
#define BAT_RAW_MAX        2430   // ≈ full charge (also the charging-detect threshold)
