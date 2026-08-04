// OpenBricx Deck — BLE HID keyboard (NimBLE HID-over-GATT).
//
// Advertises as a Bluetooth HID keyboard so the deck can send keystrokes + media
// keys wirelessly when it isn't on USB. hid.c prefers USB whenever a host is
// mounted (tud_mounted) and falls back to these calls otherwise, so output moves
// to BLE automatically on unplug.
//
// The heavy lifting (HID GATT service, report references, protocol mode, battery
// + device-info services, bonding) is done by the IDF esp_hid component's NimBLE
// device backend (esp_hidd over CONFIG_BT_NIMBLE_HID_SERVICE). This file owns the
// NimBLE host bring-up, security config, and advertising; esp_hidd tracks the
// connection itself via its own GAP listener.

#include "ble_hid.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/hci_common.h"          // BLE_HCI_ADV_FILT_* (adv filter policy)
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"             // ble_store_util_bonded_peers
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "esp_hidd.h"
#include "esp_hid_common.h"

static const char *TAG = "ble";

// Provided by NimBLE's store/config module (CONFIG_BT_NIMBLE_NVS_PERSIST) — keeps
// bonds across reboots so a paired host reconnects without re-pairing.
void ble_store_config_init(void);

// ── HID report map: keyboard (report 1) + consumer control (report 2) ─────────
// Mirrors the USB report layout in usb_descriptors.c so host behaviour matches:
//   report 1 = 8-byte boot-style keyboard report (modifier, reserved, 6 keys)
//   report 2 = 16-bit consumer usage selector (e.g. 0xE9 vol+, 0xCD play/pause)
static const uint8_t s_report_map[] = {
    // Keyboard (Report ID 1)
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)  — modifier byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const)         — reserved byte
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array)    — up to 6 keycodes
    0xC0,              // End Collection

    // Consumer control (Report ID 2)
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (0x03FF)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (0x03FF)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data,Array)
    0xC0,              // End Collection
};

#define RPT_ID_KEYBOARD 1
#define RPT_ID_CONSUMER 2

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = s_report_map, .len = sizeof(s_report_map) },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id        = OBX_USB_VID,
    .product_id       = OBX_USB_PID,
    .version          = 0x0100,
    .device_name      = OBX_DEVICE_NAME,
    .manufacturer_name = "OpenBricx",
    .serial_number    = "0001",
    .report_maps      = s_report_maps,
    .report_maps_len  = 1,
};

static esp_hidd_dev_t *s_dev = NULL;
static uint8_t s_own_addr_type;

// Pairing mode (see ble_hid.h). false ⇒ advertise only to the bonded whitelist (or
// stay private if there are no bonds); true ⇒ advertise openly + accept a new bond.
static bool s_pairing = false;

// ── Advertising ────────────────────────────────────────────────────────────────
static int gap_event(struct ble_gap_event *event, void *arg);

// Load stored bonds into the controller whitelist and return how many there are.
// advertise() uses the count to decide whether there's anything to reconnect to.
static int load_whitelist(void)
{
    ble_addr_t peers[8];
    int num = 0;
    int rc = ble_store_util_bonded_peers(peers, &num, sizeof(peers) / sizeof(peers[0]));
    if (rc != 0) {
        ESP_LOGW(TAG, "bonded_peers rc=%d", rc);
        return 0;
    }
    if (num > 0) {
        rc = ble_gap_wl_set(peers, num);
        if (rc != 0) ESP_LOGW(TAG, "wl_set rc=%d", rc);
    }
    return num;
}

static void advertise(void)
{
    if (ble_gap_adv_active()) return;   // idempotent: safe to call from several paths
    if (ble_hid_connected()) return;    // a host is already attached — nothing to do

    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "addr infer failed: %d", rc);
        return;
    }

    // Gate discoverability on the pairing flag. Not pairing + no bonds ⇒ stay dark
    // so nothing can connect; not pairing + bonds ⇒ advertise whitelist-filtered so
    // only a previously-paired host can reconnect.
    int num_bonds = load_whitelist();
    if (!s_pairing && num_bonds == 0) {
        ESP_LOGI(TAG, "private (no bonds, pairing off) — enable Settings > Pairing to connect");
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = ESP_HID_APPEARANCE_KEYBOARD;   // 0x03C1
    fields.appearance_is_present = 1;
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);  // HID service
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (uint8_t *)OBX_DEVICE_NAME;
    fields.name_len = strlen(OBX_DEVICE_NAME);
    fields.name_is_complete = 1;
    // flags(3)+appearance(4)+uuid16(4)+name(16) = 27 B, within the 31 B adv PDU.

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;   // undirected connectable
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;   // general discoverable
    // Not pairing → filter scans + connection requests to the bonded whitelist, so
    // only a host we've paired with before can reach us; new hosts are ignored.
    if (!s_pairing)
        advp.filter_policy = BLE_HCI_ADV_FILT_BOTH;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\" (%s)", OBX_DEVICE_NAME,
             s_pairing ? "pairing — open to new hosts" : "bonded-only");
}

// Re-apply the advertising policy after the pairing flag flips at runtime.
static void restart_advertising(void)
{
    ble_gap_adv_stop();   // rc ignored: EALREADY if it wasn't running
    advertise();
}

// esp_hidd registers its own GAP listener for connect/disconnect bookkeeping; this
// callback owns the advertising lifecycle and prompts encryption. HID input is
// only accepted by the host over an ENCRYPTED link, so we request security on
// connect (a no-op/EALREADY if the central is already pairing; on a bonded
// reconnect it re-establishes encryption).
static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            int rc = ble_gap_security_initiate(event->connect.conn_handle);
            ESP_LOGI(TAG, "connected; security_initiate rc=%d", rc);
        } else {
            ESP_LOGW(TAG, "connect failed (status=%d)", event->connect.status);
            advertise();
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption %s (status=%d)",
                 event->enc_change.status == 0 ? "ON" : "FAILED",
                 event->enc_change.status);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        // Host enabled notifications on a report characteristic — input will flow.
        ESP_LOGI(TAG, "subscribe attr=%d cur_notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

// ── esp_hidd device events ─────────────────────────────────────────────────────
static void hidd_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:        // host synced — begin advertising
        advertise();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "host connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:   // re-advertise so the host can reconnect
        ESP_LOGI(TAG, "host disconnected — re-advertising");
        advertise();
        break;
    default:
        break;
    }
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();                 // returns only at nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ── Public API (see ble_hid.h) ─────────────────────────────────────────────────
void ble_hid_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d — BLE HID disabled", err);
        return;
    }

    // Bondable Just-Works pairing with LE Secure Connections. Windows bonds with a
    // HID peripheral before it will accept input reports; no IO means no PIN.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    // Registers the HID GATT service + its own GAP listener + the sync callback
    // (which posts ESP_HIDD_START_EVENT, where we start advertising). NOTE: this
    // calls ble_svc_gap_init() internally, which RESETS the GAP device-name
    // characteristic to the build default ("nimble") — so the name must be set
    // AFTER this, not before, or hosts display "nimble".
    err = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_cb, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %d — BLE HID disabled", err);
        return;
    }

    int rc = ble_svc_gap_device_name_set(OBX_DEVICE_NAME);
    if (rc != 0) ESP_LOGW(TAG, "set device name failed: %d", rc);

    ble_store_config_init();           // persist bonds in NVS
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE HID (NimBLE) up — keyboard advertising once host syncs");
}

bool ble_hid_connected(void)
{
    return s_dev != NULL && esp_hidd_dev_connected(s_dev);
}

void ble_hid_keyboard_report(uint8_t mods, uint8_t keycode)
{
    if (!ble_hid_connected()) return;
    uint8_t report[8] = { mods, 0, keycode, 0, 0, 0, 0, 0 };
    esp_err_t rc = esp_hidd_dev_input_set(s_dev, 0, RPT_ID_KEYBOARD, report, sizeof(report));
    if (rc != ESP_OK) ESP_LOGW(TAG, "kbd report rc=%d (link not ready?)", rc);
}

void ble_hid_consumer_report(uint16_t usage)
{
    if (!ble_hid_connected()) return;
    uint8_t report[2] = { (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
    esp_err_t rc = esp_hidd_dev_input_set(s_dev, 0, RPT_ID_CONSUMER, report, sizeof(report));
    if (rc != ESP_OK) ESP_LOGW(TAG, "consumer report rc=%d (link not ready?)", rc);
}

void ble_hid_set_battery(uint8_t percent)
{
    if (s_dev == NULL) return;
    esp_hidd_dev_battery_set(s_dev, percent);
}

void ble_hid_set_pairing(bool on)
{
    if (s_pairing == on) return;
    s_pairing = on;
    if (on) {
        // Pairing is an explicit "set up this host" action, so start from a clean
        // slate: wipe every stored bond/CCCD first. A *mismatched* bond is what
        // produces the connect/disconnect loop — if the host was removed and
        // re-added on its side, the deck still holds the old LTK and tries to
        // encrypt with a key the host no longer has; the host can't, so it drops
        // the link and immediately retries, forever. Clearing the deck side (plus
        // removing the deck on the host) guarantees the fresh pair can't collide.
        int rc = ble_store_clear();
        ESP_LOGI(TAG, "pairing ON — bonds cleared (rc=%d), discoverable", rc);
    } else {
        ESP_LOGI(TAG, "pairing OFF — bonded-only");
    }
    restart_advertising();
}

bool ble_hid_pairing(void) { return s_pairing; }
