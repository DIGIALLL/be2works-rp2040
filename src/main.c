// Эмулятор USB-SMBus моста Silicon Labs CP2112 на RP2040 (RP2040-Zero).
// Компьютер видит устройство 10C4:EA90, поэтому с ним работают программы,
// написанные под CP2112 (Be2Works, SLABHIDtoSMBus.dll, драйвер Linux hid-cp2112).

#include "pico/stdlib.h"
#include "tusb.h"
#include "cp2112.h"

int main(void) {
    cp2112_init();
    tusb_init();

    for (;;) {
        tud_task();
        cp2112_task();
    }
}

// ---------------------------------------------------------------------------
// HID-колбэки TinyUSB

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    if (report_type == HID_REPORT_TYPE_INPUT) return cp2112_get_input(report_id, buffer, reqlen);
    if (report_type != HID_REPORT_TYPE_FEATURE) return 0;
    return cp2112_get_feature(report_id, buffer, reqlen);
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize) {
    (void)instance;

    // С interrupt OUT эндпоинта TinyUSB передаёт report_id = 0, а сам ID — первым байтом
    if (report_id == 0) {
        if (bufsize == 0) return;
        report_id = buffer[0];
        buffer++;
        bufsize--;
    }

    if (report_type == HID_REPORT_TYPE_FEATURE)
        cp2112_set_feature(report_id, buffer, bufsize);
    else
        cp2112_on_output(report_id, buffer, bufsize);
}
