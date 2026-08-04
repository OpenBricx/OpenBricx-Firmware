// OpenBricx Deck — firmware identity & tunables.
#pragma once

// ── OBX device identity (sent in the OBX-HELLO handshake) ─────────────────────
#define OBX_PROTOCOL_VERSION 1
#define OBX_PRODUCT          "openbricx-deck"   // must match the plugin manifest
#define OBX_DEVICE_NAME      "OpenBricx Deck"
#define OBX_CHIP             "esp32s3"
#define OBX_HW_REV           "1.0"

#ifndef OBX_FW_VERSION
#define OBX_FW_VERSION       "1.8.0"
#endif

// USB identifiers — Espressif VID + app-mode PID (matches the old companion's
// app-mode filter so existing tooling still recognises the device).
#define OBX_USB_VID          0x303A
#define OBX_USB_PID          0x1001

// ── Macro engine ──────────────────────────────────────────────────────────────
#define DECK_MAX_PAGES       4
#define DECK_BUTTON_COUNT    9     // 3x3 matrix; encoder click is logical button 10
#define DECK_MAX_TEXT_LEN    64

// ── Timing ────────────────────────────────────────────────────────────────────
#define DECK_TASK_PERIOD_MS  5
#define DECK_BATTERY_PERIOD_MS 30000   // battery sample + BLE notify + power-source check cadence
#define DECK_WDT_TIMEOUT_S   8
