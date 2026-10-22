#pragma once
#include <stdint.h>
#include <stdbool.h>
bool tud_hid_ready(void); bool tud_hid_report(uint8_t id, const void *d, uint16_t n); void tud_disconnect(void);
