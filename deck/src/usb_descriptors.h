// OpenBricx Deck — USB composite device (CDC-ACM + HID).
#pragma once
#include <stdint.h>

// HID report IDs (must match the report descriptor in usb_descriptors.c).
enum {
    HID_REPORT_ID_KEYBOARD = 1,
    HID_REPORT_ID_CONSUMER = 2,
};

// Initialise TinyUSB with our composite descriptors (CDC + HID) and start the
// CDC-ACM serial interface. Safe to call once from app_main.
void usb_init(void);

// True once a USB host has enumerated the device (TinyUSB mounted). This is the
// reliable "USB present" signal used to pick USB vs BLE transport.
bool usb_mounted(void);
