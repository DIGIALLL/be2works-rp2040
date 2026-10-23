#include "tusb.h"
#include "config.h"
#include "cp2112.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Device

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CP2112_VID,
    .idProduct          = CP2112_PID,
    .bcdDevice          = 0x0000,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

// ---------------------------------------------------------------------------
// HID report descriptor: vendor page 0xFF00, все репорты — байтовые массивы

#define RPT(id, count, kind) \
    0x85, (id), 0x95, (count), 0x75, 0x08, 0x26, 0xFF, 0x00, 0x15, 0x00, 0x09, 0x01, (kind), 0x02

#define FEATURE 0xB1
#define OUTPUT  0x91
#define INPUT   0x81

static const uint8_t desc_hid_report[] = {
    0x06, 0x00, 0xFF,   // Usage Page (Vendor 0xFF00)
    0x09, 0x01,         // Usage (1)
    0xA1, 0x01,         // Collection (Application)

    RPT(RPT_RESET,                LEN_RESET,            FEATURE),
    RPT(RPT_GPIO_CONFIG,          LEN_GPIO_CONFIG,      FEATURE),
    RPT(RPT_GPIO_GET,             LEN_GPIO_GET,         FEATURE),
    RPT(RPT_GPIO_SET,             LEN_GPIO_SET,         FEATURE),
    RPT(RPT_VERSION,              LEN_VERSION,          FEATURE),
    RPT(RPT_SMBUS_CONFIG,         LEN_SMBUS_CONFIG,     FEATURE),
    RPT(RPT_READ_REQUEST,         LEN_READ_REQUEST,     OUTPUT),
    RPT(RPT_WRITE_READ_REQUEST,   LEN_WRITE_READ_REQ,   OUTPUT),
    RPT(RPT_READ_FORCE_SEND,      LEN_READ_FORCE_SEND,  OUTPUT),
    RPT(RPT_READ_RESPONSE,        LEN_READ_RESPONSE,    INPUT),
    RPT(RPT_WRITE,                LEN_WRITE,            OUTPUT),
    RPT(RPT_XFER_STATUS_REQUEST,  LEN_XFER_STATUS_REQ,  OUTPUT),
    RPT(RPT_XFER_STATUS_RESPONSE, LEN_XFER_STATUS_RESP, INPUT),
    RPT(RPT_CANCEL_TRANSFER,      LEN_CANCEL,           OUTPUT),
    RPT(RPT_LOCK_BYTE,            LEN_LOCK_BYTE,        FEATURE),
    RPT(RPT_USB_CONFIG,           LEN_USB_CONFIG,       FEATURE),
    RPT(RPT_MANUFACTURER_STRING,  LEN_STRING,           FEATURE),
    RPT(RPT_PRODUCT_STRING,       LEN_STRING,           FEATURE),
    RPT(RPT_SERIAL_STRING,        LEN_STRING,           FEATURE),
    RPT(RPT_DEBUG_LOG,            LEN_DEBUG_LOG,        FEATURE),

    0xC0                // End Collection
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

// ---------------------------------------------------------------------------
// Configuration: один HID-интерфейс с interrupt IN и OUT, как у CP2112

#define EPNUM_HID_OUT   0x01
#define EPNUM_HID_IN    0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report),
                             EPNUM_HID_OUT, EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 1),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ---------------------------------------------------------------------------
// Strings

static uint16_t desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        const char *str = cp2112_usb_string(index);
        if (!str) return NULL;
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) desc_str[1 + i] = (uint8_t)str[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
