#ifndef CONFIG_H
#define CONFIG_H

// Пины SMBus на RP2040-Zero (I2C0)
#define SMBUS_I2C           i2c0
#define SMBUS_SDA_PIN       4
#define SMBUS_SCL_PIN       5

// Встроенные подтяжки RP2040 (~50 кОм) слабые; для батареи нужны внешние 4.7 кОм к 3.3 В
#define SMBUS_INTERNAL_PULLUPS 1

#define SMBUS_DEFAULT_HZ    100000

// Если таймаут в конфигурации SMBus равен 0 ("без таймаута"), используем этот
#define SMBUS_FALLBACK_TIMEOUT_MS 1000

// Максимум повторов при NACK, даже если хост попросил больше
#define SMBUS_MAX_RETRIES   3

#define CP2112_VID          0x10C4
#define CP2112_PID          0xEA90
#define CP2112_PART_NUMBER  0x0C
#define CP2112_DEVICE_VER   0x02

#endif
