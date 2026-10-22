#include "smbus.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

static uint32_t current_hz = SMBUS_DEFAULT_HZ;

static void pins_to_i2c(void) {
    gpio_set_function(SMBUS_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SMBUS_SCL_PIN, GPIO_FUNC_I2C);
#if SMBUS_INTERNAL_PULLUPS
    gpio_pull_up(SMBUS_SDA_PIN);
    gpio_pull_up(SMBUS_SCL_PIN);
#endif
}

// Выводим ведомое устройство из зависшего состояния: до 9 тактов SCL и STOP
static void bus_recover(void) {
    i2c_deinit(SMBUS_I2C);

    gpio_init(SMBUS_SDA_PIN);
    gpio_init(SMBUS_SCL_PIN);
    gpio_set_dir(SMBUS_SDA_PIN, GPIO_IN);
    gpio_put(SMBUS_SCL_PIN, 0);
    gpio_put(SMBUS_SDA_PIN, 0);

    // Open-drain эмулируем переключением направления: выход = 0, вход = отпущено
    for (int i = 0; i < 9 && !gpio_get(SMBUS_SDA_PIN); i++) {
        gpio_set_dir(SMBUS_SCL_PIN, GPIO_OUT);
        sleep_us(5);
        gpio_set_dir(SMBUS_SCL_PIN, GPIO_IN);
        sleep_us(5);
    }

    // STOP: SDA 0 -> 1 при SCL = 1
    gpio_set_dir(SMBUS_SDA_PIN, GPIO_OUT);
    sleep_us(5);
    gpio_set_dir(SMBUS_SCL_PIN, GPIO_IN);
    sleep_us(5);
    gpio_set_dir(SMBUS_SDA_PIN, GPIO_IN);
    sleep_us(5);

    i2c_init(SMBUS_I2C, current_hz);
    pins_to_i2c();
}

void smbus_init(uint32_t hz) {
    current_hz = hz;
    i2c_init(SMBUS_I2C, hz);
    pins_to_i2c();
}

void smbus_set_clock(uint32_t hz) {
    if (hz < 10000) hz = 10000;
    if (hz > 400000) hz = 400000;
    if (hz == current_hz) return;
    current_hz = hz;
    i2c_set_baudrate(SMBUS_I2C, hz);
}

static uint32_t to_us(uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = SMBUS_FALLBACK_TIMEOUT_MS;
    return timeout_ms * 1000u;
}

static smbus_result_t map_write(int rc, size_t len) {
    if (rc == (int)len) return SMBUS_OK;
    if (rc == PICO_ERROR_TIMEOUT) {
        bus_recover();
        return SMBUS_BUS_TIMEOUT;
    }
    if (rc >= 0) return SMBUS_DATA_NACK;
    return SMBUS_ADDR_NACK;
}

static smbus_result_t map_read(int rc, size_t len) {
    if (rc == (int)len) return SMBUS_OK;
    if (rc == PICO_ERROR_TIMEOUT) {
        bus_recover();
        return SMBUS_BUS_TIMEOUT;
    }
    return SMBUS_ADDR_NACK;
}

smbus_result_t smbus_write(uint8_t addr, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    int rc = i2c_write_timeout_us(SMBUS_I2C, addr, data, len, false, to_us(timeout_ms));
    return map_write(rc, len);
}

smbus_result_t smbus_read(uint8_t addr, uint8_t *data, size_t len, uint32_t timeout_ms) {
    int rc = i2c_read_timeout_us(SMBUS_I2C, addr, data, len, false, to_us(timeout_ms));
    return map_read(rc, len);
}

smbus_result_t smbus_write_read(uint8_t addr, const uint8_t *target, size_t target_len,
                                uint8_t *data, size_t len, uint32_t timeout_ms) {
    int rc = i2c_write_timeout_us(SMBUS_I2C, addr, target, target_len, true, to_us(timeout_ms));
    smbus_result_t res = map_write(rc, target_len);
    if (res != SMBUS_OK) {
        // После неудачной записи с nostop контроллер сам выдаёт STOP при abort
        return res;
    }
    rc = i2c_read_timeout_us(SMBUS_I2C, addr, data, len, false, to_us(timeout_ms));
    return map_read(rc, len);
}
