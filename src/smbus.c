#include "smbus.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

static uint32_t current_hz = SMBUS_DEFAULT_HZ;

static void pins_to_i2c(void) {
#if SMBUS_HW_I2C
    gpio_set_function(SMBUS_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SMBUS_SCL_PIN, GPIO_FUNC_I2C);
#else
    // Программная шина: open-drain эмулируем направлением, выход всегда 0
    gpio_init(SMBUS_SDA_PIN);
    gpio_init(SMBUS_SCL_PIN);
    gpio_put(SMBUS_SDA_PIN, 0);
    gpio_put(SMBUS_SCL_PIN, 0);
#endif
#if SMBUS_INTERNAL_PULLUPS
    gpio_pull_up(SMBUS_SDA_PIN);
    gpio_pull_up(SMBUS_SCL_PIN);
#endif
}

// Выводим ведомое устройство из зависшего состояния: до 9 тактов SCL и STOP
static void bus_recover(void) {
#if SMBUS_HW_I2C
    i2c_deinit(SMBUS_I2C);
#endif

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

#if SMBUS_HW_I2C
    i2c_init(SMBUS_I2C, current_hz);
#endif
    pins_to_i2c();
}

static uint32_t to_us(uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = SMBUS_FALLBACK_TIMEOUT_MS;
    return timeout_ms * 1000u;
}

static uint32_t clamp_hz(uint32_t hz) {
    if (hz < 10000) hz = 10000;
    if (hz > 400000) hz = 400000;
    return hz;
}

#if SMBUS_HW_I2C

void smbus_init(uint32_t hz) {
    current_hz = clamp_hz(hz);
    i2c_init(SMBUS_I2C, current_hz);
    pins_to_i2c();
}

void smbus_set_clock(uint32_t hz) {
    hz = clamp_hz(hz);
    if (hz == current_hz) return;
    current_hz = hz;
    i2c_set_baudrate(SMBUS_I2C, hz);
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
        // При abort контроллер сам выдаёт STOP, но SDK оставляет флаг повторного START
        SMBUS_I2C->restart_on_next = false;
        return res;
    }
    rc = i2c_read_timeout_us(SMBUS_I2C, addr, data, len, false, to_us(timeout_ms));
    return map_read(rc, len);
}

#else  // программный I2C

static uint32_t half_period_us = 5;
static absolute_time_t deadline;

static void line_low(uint pin) { gpio_set_dir(pin, GPIO_OUT); }
static void line_release(uint pin) { gpio_set_dir(pin, GPIO_IN); }
static void half_delay(void) { busy_wait_us_32(half_period_us); }

// Отпускаем SCL и ждём, пока ведомый закончит clock stretching
static bool scl_high(void) {
    line_release(SMBUS_SCL_PIN);
    while (!gpio_get(SMBUS_SCL_PIN)) {
        if (time_reached(deadline)) return false;
    }
    return true;
}

// START и повторный START: SDA 1 -> 0 при SCL = 1
static bool bus_start(void) {
    line_release(SMBUS_SDA_PIN);
    half_delay();
    if (!scl_high()) return false;
    half_delay();
    if (!gpio_get(SMBUS_SDA_PIN)) return false;  // SDA держит кто-то другой
    line_low(SMBUS_SDA_PIN);
    half_delay();
    line_low(SMBUS_SCL_PIN);
    return true;
}

// STOP: SDA 0 -> 1 при SCL = 1
static void bus_stop(void) {
    line_low(SMBUS_SDA_PIN);
    half_delay();
    scl_high();
    half_delay();
    line_release(SMBUS_SDA_PIN);
    half_delay();
}

// 1 = ACK, 0 = NACK, -1 = таймаут clock stretching
static int write_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) line_release(SMBUS_SDA_PIN);
        else line_low(SMBUS_SDA_PIN);
        half_delay();
        if (!scl_high()) return -1;
        half_delay();
        line_low(SMBUS_SCL_PIN);
    }
    line_release(SMBUS_SDA_PIN);
    half_delay();
    if (!scl_high()) return -1;
    int ack = !gpio_get(SMBUS_SDA_PIN);
    half_delay();
    line_low(SMBUS_SCL_PIN);
    return ack;
}

// ack = true — подтверждаем байт (будут ещё), false — NACK на последнем
static bool read_byte(uint8_t *out, bool ack) {
    uint8_t v = 0;
    line_release(SMBUS_SDA_PIN);
    for (int i = 0; i < 8; i++) {
        half_delay();
        if (!scl_high()) return false;
        v = (uint8_t)((v << 1) | gpio_get(SMBUS_SDA_PIN));
        half_delay();
        line_low(SMBUS_SCL_PIN);
    }
    if (ack) line_low(SMBUS_SDA_PIN);
    half_delay();
    if (!scl_high()) return false;
    half_delay();
    line_low(SMBUS_SCL_PIN);
    line_release(SMBUS_SDA_PIN);
    *out = v;
    return true;
}

// Запись wr (если есть), повторный START, чтение rd (если есть), STOP
static smbus_result_t transfer(uint8_t addr, const uint8_t *wr, size_t wr_len,
                               uint8_t *rd, size_t rd_len, uint32_t timeout_ms) {
    deadline = make_timeout_time_us(to_us(timeout_ms));
    smbus_result_t res = SMBUS_OK;
    int ack;

    if (!bus_start()) { res = SMBUS_BUS_TIMEOUT; goto done; }

    if (wr_len || !rd_len) {
        ack = write_byte((uint8_t)(addr << 1));
        if (ack < 0) { res = SMBUS_BUS_TIMEOUT; goto done; }
        if (!ack) { res = SMBUS_ADDR_NACK; goto done; }
        for (size_t i = 0; i < wr_len; i++) {
            ack = write_byte(wr[i]);
            if (ack < 0) { res = SMBUS_BUS_TIMEOUT; goto done; }
            if (!ack) { res = SMBUS_DATA_NACK; goto done; }
        }
        if (rd_len && !bus_start()) { res = SMBUS_BUS_TIMEOUT; goto done; }
    }

    if (rd_len) {
        ack = write_byte((uint8_t)((addr << 1) | 1));
        if (ack < 0) { res = SMBUS_BUS_TIMEOUT; goto done; }
        if (!ack) { res = SMBUS_ADDR_NACK; goto done; }
        for (size_t i = 0; i < rd_len; i++) {
            if (!read_byte(&rd[i], i + 1 < rd_len)) { res = SMBUS_BUS_TIMEOUT; goto done; }
        }
    }

done:
    bus_stop();
    if (res == SMBUS_BUS_TIMEOUT) bus_recover();
    return res;
}

void smbus_init(uint32_t hz) {
    pins_to_i2c();
    current_hz = 0;
    smbus_set_clock(hz);
}

void smbus_set_clock(uint32_t hz) {
    hz = clamp_hz(hz);
    if (hz == current_hz) return;
    current_hz = hz;
    half_period_us = 500000u / hz;
    if (half_period_us == 0) half_period_us = 1;
}

smbus_result_t smbus_write(uint8_t addr, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    return transfer(addr, data, len, NULL, 0, timeout_ms);
}

smbus_result_t smbus_read(uint8_t addr, uint8_t *data, size_t len, uint32_t timeout_ms) {
    return transfer(addr, NULL, 0, data, len, timeout_ms);
}

smbus_result_t smbus_write_read(uint8_t addr, const uint8_t *target, size_t target_len,
                                uint8_t *data, size_t len, uint32_t timeout_ms) {
    return transfer(addr, target, target_len, data, len, timeout_ms);
}

#endif
