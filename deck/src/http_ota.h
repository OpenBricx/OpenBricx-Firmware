// OpenBricx Deck — HTTP firmware-update endpoint (real OTA, over Wi-Fi).
#pragma once

// Start the HTTP server exposing `POST /obx/ota`, which streams the request body
// into the inactive OTA slot and reboots into it. Idempotent — safe to call again;
// only the first call starts the server.
void http_ota_start(void);

// Stop the HTTP server (when the AP is taken down). Idempotent.
void http_ota_stop(void);
