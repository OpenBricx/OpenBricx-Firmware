// OpenBricx Deck — Wi-Fi SoftAP for OTA. See wifi.h / http_ota.h.

#include "wifi.h"
#include "http_ota.h"
#include "obx_protocol.h"

#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "wifi";

// AP identity. WPA2 needs a >= 8-char password; set AP_PASS to "" for an open AP.
#define AP_SSID    "OpenBricx-Deck"
#define AP_PASS    "openbricx"
#define AP_CHANNEL 6
#define AP_IP      "192.168.4.1"
#define AP_MAX_STA 2
#define AP_TX_POWER 8      // 2 dBm in 0.25 dBm units; minimises LDO current spike on battery

static bool s_inited = false;
static bool s_ap_on = false;
static int s_clients = 0;
static char s_status[48] = "idle";
static int8_t s_tx = 0;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_clients++;
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_clients > 0) s_clients--;
    }
}

void wifi_init(void)
{
    if (s_inited) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // Radio stays off until wifi_ap_start().
    s_inited = true;
}

// Report a step result over CDC and, on failure, latch it into s_status so the
// Update screen can show which call broke. Returns true on ESP_OK.
static bool step_ok(const char *what, esp_err_t e)
{
    char line[64];
    snprintf(line, sizeof(line), "WIFI: %s = %s", what, esp_err_to_name(e));
    obx_serial_println(line);
    if (e != ESP_OK) {
        snprintf(s_status, sizeof(s_status), "FAIL %s:%s", what, esp_err_to_name(e));
    }
    return e == ESP_OK;
}

void wifi_ap_start(void)
{
    if (s_ap_on) return;

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password) - 1);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_STA;
    ap.ap.authmode = (strlen(AP_PASS) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    // Non-fatal: if a step fails we want to SEE which one (on serial + the Update
    // screen), not silently abort/reboot the way ESP_ERROR_CHECK would.
    if (!step_ok("set_mode",   esp_wifi_set_mode(WIFI_MODE_AP)))        return;
    if (!step_ok("set_config", esp_wifi_set_config(WIFI_IF_AP, &ap)))   return;
    if (!step_ok("start",      esp_wifi_start()))                       return;

    // The S3 SuperMini's tiny PCB antenna + small LDO can't sustain the default
    // 20 dBm: at full power the beacon never radiates cleanly and the SSID never
    // shows up. Cap TX power to ~8.5 dBm (34 * 0.25 dBm) — must be set AFTER
    // esp_wifi_start(). Read it back so the screen shows what actually took.
    esp_err_t te = esp_wifi_set_max_tx_power(AP_TX_POWER);
    step_ok("set_tx_power", te);
    s_tx = 0;
    esp_wifi_get_max_tx_power(&s_tx);

    snprintf(s_status, sizeof(s_status), "AP OK tx=%d", (int)s_tx);
    obx_serial_println(s_status);

    s_clients = 0;
    http_ota_start();          // POST /obx/ota reachable at AP_IP now
    s_ap_on = true;
    ESP_LOGI(TAG, "AP up: \"%s\" @ %s ch=%d tx=%d", AP_SSID, AP_IP, AP_CHANNEL, (int)s_tx);
}

void wifi_ap_stop(void)
{
    if (!s_ap_on) return;
    http_ota_stop();
    esp_wifi_stop();
    s_ap_on = false;
    s_clients = 0;
    snprintf(s_status, sizeof(s_status), "idle");
    ESP_LOGI(TAG, "AP down");
}

bool wifi_ap_active(void) { return s_ap_on; }
int  wifi_ap_clients(void) { return s_clients; }
const char *wifi_ap_ssid(void) { return AP_SSID; }
const char *wifi_ap_pass(void) { return AP_PASS; }
const char *wifi_ap_ip(void)   { return AP_IP; }
const char *wifi_ap_status(void) { return s_status; }
int wifi_ap_tx_power(void) { return (int)s_tx; }
