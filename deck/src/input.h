// OpenBricx Deck — PCF8574 button matrix, touch sensor, encoder switch.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Initialise I2C + PCF8574. Returns false if the expander isn't found.
bool input_init(void);

// Scan the 3x3 matrix once. Fills `pressed[9]` with edge-detected presses
// (true only on the transition to pressed). Call every deck task tick.
void input_scan_matrix(bool pressed_out[9]);

// Raw current state of the encoder push switch (true = pressed).
bool input_encoder_switch(void);

// Raw current state of the TTP223 touch pad (true = touched).
bool input_touch(void);

// True if input_init() found the PCF8574 (last init result).
bool input_ready(void);

// Read the raw PCF8574 port byte (all pins idle-high). Returns false if the I2C
// read failed (no ACK / bus error) — used by the boot/DIAG diagnostics to tell a
// missing expander apart from a wiring/level problem. On success `*value` holds
// the port byte.
bool input_read_raw(uint8_t *value);

// Live (non-edge-detected) matrix scan for diagnostics: fills `pressed[9]` with
// the current held state of each button. Unlike input_scan_matrix this reports
// the instantaneous state, so a key held down shows up on a DIAG re-read.
void input_debug_scan(bool pressed[9]);

// Diagnostics: probe every 7-bit I2C address (0x08–0x77) on the configured bus
// and fill `addrs` with the addresses that ACK. Returns the count. Lets DIAG show
// whether the expander is on the bus at all and at what address (a PCF8574A sits
// at 0x38, not 0x20). Safe to call even if input_init() reported the chip missing.
int input_bus_scan(uint8_t *addrs, int max);

// Diagnostics: drive each row low in turn and return the raw PCF8574 port byte
// read back for that row in rows_out[0..2]. The column bits (P1-3) read 1 when
// idle and flip to 0 when a key in that row is pressed; P0 (touch) and P7 (enc
// switch) appear too. Lets the boot monitor show exactly what the expander sees.
void input_scan_raw(uint8_t rows_out[3]);
