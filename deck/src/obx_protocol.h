// OpenBricx Deck — OBX serial protocol (handshake + line dispatch over USB CDC).
#pragma once
#include <stdbool.h>
#include <stdint.h>

void obx_init(void);

// Drain any received CDC bytes, assemble lines, and dispatch them. Call often
// from the main deck task.
void obx_poll(void);

// Write a line (a trailing newline is added) to the host over CDC.
void obx_serial_println(const char *str);

// Emit a host-handled action request: `E<page>:<btn>`.
void obx_emit_pc_action(uint8_t page, uint8_t btn);

// Emit a local profile change (encoder long-press): `PROF_SYNC:<page>`.
void obx_emit_profile_sync(uint8_t page);

// Emit a one-shot boot/peripheral diagnostics block over CDC. `display_ok` and
// `input_ok` are the results of the respective init calls. Also re-runnable on
// demand via the `DIAG` command. Lines are prefixed `DIAG:` so the host can
// filter them from protocol traffic.
void obx_emit_boot_diag(bool display_ok, bool input_ok);
