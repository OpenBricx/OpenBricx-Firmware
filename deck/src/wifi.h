// OpenBricx Deck — Wi-Fi SoftAP for firmware updates (OTA).
//
// The deck hosts its own access point on demand (entered from the on-device
// Settings → Update screen). The PC joins it and POSTs the new image to the HTTP
// OTA endpoint at 192.168.4.1. No station mode, no network credentials, no
// scanning — the whole class of provisioning failures goes away.
#pragma once
#include <stdbool.h>

// Bring up the Wi-Fi stack (netif + event loop + esp_wifi) without starting the
// radio. Call once at boot. The AP is started/stopped on demand below.
void wifi_init(void);

// Start / stop the SoftAP (and the HTTP OTA server). Idempotent.
void wifi_ap_start(void);
void wifi_ap_stop(void);

// True while the AP is up.
bool wifi_ap_active(void);

// Number of stations currently joined to the AP (for the on-device UI).
int wifi_ap_clients(void);

// AP identity, for display on the device screen.
const char *wifi_ap_ssid(void);
const char *wifi_ap_pass(void);
const char *wifi_ap_ip(void);

// Diagnostics for the Update screen: a short status string describing the last
// AP bring-up ("AP OK tx=34" or "FAIL start:<err>"), and the TX power the radio
// actually applied (in 0.25 dBm units, so /4 for dBm). Lets us see whether the
// AP initialised at all and at what power, without a serial monitor.
const char *wifi_ap_status(void);
int wifi_ap_tx_power(void);
