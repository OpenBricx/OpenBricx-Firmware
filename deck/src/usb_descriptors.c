// OpenBricx Deck — USB composite descriptors + TinyUSB bring-up.
//
// Layout: one HID interface (boot keyboard + consumer control, two report IDs)
// and one CDC-ACM interface pair (notification + data) for the OBX serial
// channel. Descriptors are handed to esp_tinyusb via tinyusb_config_t.

#include "usb_descriptors.h"
#include "config.h"

#include "tinyusb.h"
#include "tusb.h"
#include "esp_log.h"

static const char *TAG = "usb";

// ── HID report descriptor: keyboard (ID 1) + consumer control (ID 2) ──────────
static const uint8_t hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(HID_REPORT_ID_CONSUMER)),
};

// ── Device descriptor ─────────────────────────────────────────────────────────
static const tusb_desc_device_t device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // Misc / IAD so Windows treats CDC+HID as one composite device.
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = OBX_USB_VID,
    .idProduct          = OBX_USB_PID,
    .bcdDevice          = 0x0110,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// ── Interface / endpoint numbering ────────────────────────────────────────────
enum {
    ITF_NUM_HID = 0,
    ITF_NUM_CDC,       // CDC control
    ITF_NUM_CDC_DATA,  // CDC data
    ITF_NUM_TOTAL,
};

#define EPNUM_HID       0x81
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT   0x03
#define EPNUM_CDC_IN    0x83

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t config_desc[] = {
    // Config: number of interfaces, string index, total length, attributes, power (mA)
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // HID: interface, string, boot protocol, report desc len, EP in, size, poll (ms)
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_desc), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 5),

    // CDC: interface, string, notif EP, notif size, data out EP, data in EP, size
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

// ── String descriptors ────────────────────────────────────────────────────────
static const char *string_desc[] = {
    (const char[]){0x09, 0x04},  // 0: language (English)
    "OpenBricx",                 // 1: manufacturer
    OBX_DEVICE_NAME,             // 2: product
    "OBX-DECK",                  // 3: serial (overwritten with MAC at runtime)
    "OBX Serial",                // 4: CDC interface
};

// ── TinyUSB HID callbacks ─────────────────────────────────────────────────────
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;  // no GET_REPORT payloads to serve
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
    // Host LED state (caps/num lock) — unused.
}

// ── Bring-up ──────────────────────────────────────────────────────────────────
void usb_init(void)
{
    const tinyusb_config_t cfg = {
        .device_descriptor        = &device_desc,
        .string_descriptor        = string_desc,
        .string_descriptor_count  = sizeof(string_desc) / sizeof(string_desc[0]),
        .external_phy             = false,
        .configuration_descriptor = config_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "TinyUSB composite (CDC + HID) installed");
}

bool usb_mounted(void)
{
    return tud_mounted();
}
