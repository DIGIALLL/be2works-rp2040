#ifndef SMBUS_H
#define SMBUS_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    SMBUS_OK = 0,
    SMBUS_ADDR_NACK,        // устройство не ответило на адрес
    SMBUS_DATA_NACK,        // адрес принят, данные — нет
    SMBUS_BUS_TIMEOUT,      // шина занята / завис clock stretching
} smbus_result_t;

void smbus_init(uint32_t hz);
void smbus_set_clock(uint32_t hz);

// addr — 7-битный адрес
smbus_result_t smbus_write(uint8_t addr, const uint8_t *data, size_t len, uint32_t timeout_ms);
smbus_result_t smbus_read(uint8_t addr, uint8_t *data, size_t len, uint32_t timeout_ms);
// Запись target, повторный START, чтение len байт
smbus_result_t smbus_write_read(uint8_t addr, const uint8_t *target, size_t target_len,
                                uint8_t *data, size_t len, uint32_t timeout_ms);

#endif
