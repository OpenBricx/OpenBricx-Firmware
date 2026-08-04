// OpenBricx Deck — BLE HID keyboard (NimBLE HID-over-GATT).
//
// Implemented in ble_hid.c over the IDF esp_hid component's NimBLE device backend.
// The deck advertises as a Bluetooth keyboard; hid.c routes output here whenever a
// USB host isn't mounted, so keys/media move to BLE automatically on unplug.
//
// Modifier byte and consumer usage codes match the USB path (see hid.c).
#pragma once
#include <stdint.h>
#include <stdbool.h>

void ble_hid_init(void);

// True once a BLE central (host) is connected and HID is ready.
bool ble_hid_connected(void);

// Send a keyboard report (one keycode + modifier byte). keycode 0 = release.
void ble_hid_keyboard_report(uint8_t mods, uint8_t keycode);

// Send a consumer-control report (16-bit usage; 0 = release).
void ble_hid_consumer_report(uint16_t usage);

// Report the current battery level to connected hosts (BLE Battery Service).
void ble_hid_set_battery(uint8_t percent);

// Pairing mode. When OFF (default) the deck only advertises to already-bonded
// hosts (whitelist-filtered), so a brand-new host can't connect and therefore
// can't pair; if nothing is bonded it stays private. When ON it advertises openly
// and accepts a new bond. Driven by Settings → Pairing (bridged in main.c).
void ble_hid_set_pairing(bool on);
bool ble_hid_pairing(void);
