#ifndef CONFIG_H
#define CONFIG_H

// Пины SMBus на RP2040-Zero. Можно задать любые; при сборке можно переопределить через -D
#ifndef SMBUS_SDA_PIN
#define SMBUS_SDA_PIN       3
#endif
#ifndef SMBUS_SCL_PIN
#define SMBUS_SCL_PIN       2
#endif

// Аппаратный I2C есть только у пар, где SDA на чётном пине, SCL на нечётном и оба
// пина одного блока: GPn принадлежит I2C((n / 2) % 2). Иначе шина работает программно
#if (SMBUS_SDA_PIN % 2 == 0) && (SMBUS_SCL_PIN % 2 == 1) && \
    ((SMBUS_SDA_PIN / 2) % 2 == (SMBUS_SCL_PIN / 2) % 2)
#define SMBUS_HW_I2C        1
#define SMBUS_I2C           (((SMBUS_SDA_PIN / 2) % 2) ? i2c1 : i2c0)
#else
#define SMBUS_HW_I2C        0
#endif

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
