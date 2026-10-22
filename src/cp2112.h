#ifndef CP2112_H
#define CP2112_H

#include <stdint.h>

// Идентификаторы HID-репортов CP2112 (Silicon Labs AN495)
enum {
    RPT_RESET               = 0x01,
    RPT_GPIO_CONFIG         = 0x02,
    RPT_GPIO_GET            = 0x03,
    RPT_GPIO_SET            = 0x04,
    RPT_VERSION             = 0x05,
    RPT_SMBUS_CONFIG        = 0x06,
    RPT_READ_REQUEST        = 0x10,
    RPT_WRITE_READ_REQUEST  = 0x11,
    RPT_READ_FORCE_SEND     = 0x12,
    RPT_READ_RESPONSE       = 0x13,
    RPT_WRITE               = 0x14,
    RPT_XFER_STATUS_REQUEST = 0x15,
    RPT_XFER_STATUS_RESPONSE= 0x16,
    RPT_CANCEL_TRANSFER     = 0x17,
    RPT_LOCK_BYTE           = 0x20,
    RPT_USB_CONFIG          = 0x21,
    RPT_MANUFACTURER_STRING = 0x22,
    RPT_PRODUCT_STRING      = 0x23,
    RPT_SERIAL_STRING       = 0x24,
};

// Размеры полезной нагрузки (без байта Report ID) — должны совпадать с HID-дескриптором
#define LEN_RESET            1
#define LEN_GPIO_CONFIG      4
#define LEN_GPIO_GET         1
#define LEN_GPIO_SET         2
#define LEN_VERSION          2
#define LEN_SMBUS_CONFIG     13
#define LEN_READ_REQUEST     3
#define LEN_WRITE_READ_REQ   20
#define LEN_READ_FORCE_SEND  2
#define LEN_READ_RESPONSE    63
#define LEN_WRITE            63
#define LEN_XFER_STATUS_REQ  1
#define LEN_XFER_STATUS_RESP 6
#define LEN_CANCEL           1
#define LEN_LOCK_BYTE        1
#define LEN_USB_CONFIG       9
#define LEN_STRING           63

void cp2112_init(void);
void cp2112_task(void);

uint16_t cp2112_get_feature(uint8_t id, uint8_t *buf, uint16_t maxlen);
void cp2112_set_feature(uint8_t id, const uint8_t *buf, uint16_t len);
void cp2112_on_output(uint8_t id, const uint8_t *buf, uint16_t len);

// ASCII-строки для USB-дескрипторов (1 — производитель, 2 — продукт, 3 — серийный номер)
const char *cp2112_usb_string(uint8_t index);

#endif
