#include "autoscan.h"
#include "config.h"
#include "smbus.h"

#include "pico/stdlib.h"

#define BATTERY_ADDR     0x0B
#define REG_VOLTAGE      0x09
#define EMPTY_ADDR       0x75       // по этому адресу никого нет: если ответили, шина прижата
#define PROBE_TIMEOUT_MS 30

static autoscan_state_t state = AUTOSCAN_SEARCHING;
static bool     battery_ok;
static uint8_t  cur_variant;        // какой вариант сейчас выставлен на пинах
static uint8_t  next_variant;       // какой пробовать следующим
static uint8_t  fail_count;
static uint32_t last_host_ms;
static uint32_t next_at_ms;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static bool read_voltage(uint16_t *mv) {
    const uint8_t reg = REG_VOLTAGE;
    uint8_t d[2];
    if (smbus_write_read(BATTERY_ADDR, &reg, 1, d, 2, PROBE_TIMEOUT_MS) != SMBUS_OK) return false;
    *mv = (uint16_t)(d[0] | d[1] << 8);
    return true;
}

// Тот же критерий, что у cp2112_tool.py autodetect: два одинаковых правдоподобных чтения
// напряжения и тишина по пустому адресу (прижатая шина «подтверждает» всё подряд)
static bool battery_answers(void) {
    uint16_t v1, v2;
    if (!read_voltage(&v1) || !read_voltage(&v2)) return false;
    if (smbus_write(EMPTY_ADDR, NULL, 0, PROBE_TIMEOUT_MS) == SMBUS_OK) return false;
    if (v1 < 2000 || v1 > 25000 || v2 < 2000 || v2 > 25000) return false;
    return (v1 > v2 ? v1 - v2 : v2 - v1) <= 200;
}

static void select_variant(uint8_t n) {
    if (smbus_set_variant(n)) cur_variant = n;
    else state = AUTOSCAN_OFF;      // аппаратный I2C: пины менять нельзя
}

void autoscan_host_transfer_begin(void) {
    last_host_ms = now_ms();
    // Поиск мог оставить шину на чужих пинах: пока батарея не найдена, хост работает на пинах по умолчанию
    if (state == AUTOSCAN_SEARCHING && cur_variant != 0) select_variant(0);
    next_variant = 0;
}

void autoscan_host_transfer_end(uint8_t addr, bool ok) {
    last_host_ms = now_ms();
    if (addr != BATTERY_ADDR) return;
    battery_ok = ok;
    if (ok && state == AUTOSCAN_SEARCHING) {
        state = AUTOSCAN_FOUND;     // хост сам нашёл батарею на текущих пинах
        fail_count = 0;
    }
}

void autoscan_manual_override(void) {
    state = AUTOSCAN_OFF;
    battery_ok = false;
}

void autoscan_restart(void) {
    state = AUTOSCAN_SEARCHING;
    battery_ok = false;
    fail_count = 0;
    next_variant = 0;
    next_at_ms = 0;
    select_variant(0);
}

autoscan_state_t autoscan_state(void) { return state; }
bool autoscan_battery_ok(void) { return battery_ok; }

void autoscan_task(uint32_t host_hz) {
    if (state == AUTOSCAN_OFF) return;

    uint32_t now = now_ms();
    // Хост работает с шиной — не мешаем
    if (now - last_host_ms < AUTOSCAN_HOST_IDLE_MS) return;
    if ((int32_t)(now - next_at_ms) < 0) return;

    if (state == AUTOSCAN_FOUND) {
        // Следим, что батарея на месте; три промаха подряд — ищем заново
        if (battery_answers()) {
            battery_ok = true;
            fail_count = 0;
        } else if (++fail_count >= 3) {
            battery_ok = false;
            fail_count = 0;
            state = AUTOSCAN_SEARCHING;
            next_variant = 0;
        }
        next_at_ms = now_ms() + AUTOSCAN_CHECK_MS;
        return;
    }

    // Поиск: медленная шина, чтобы хватало слабых внутренних подтяжек
    smbus_set_clock(AUTOSCAN_PROBE_HZ);
    select_variant(next_variant);
    bool found = state == AUTOSCAN_SEARCHING && battery_answers();
    smbus_set_clock(host_hz);

    if (found) {
        state = AUTOSCAN_FOUND;
        battery_ok = true;
        fail_count = 0;
    } else {
        next_variant = (uint8_t)((next_variant + 1) % AUTOSCAN_VARIANTS);
    }
    next_at_ms = now_ms() + AUTOSCAN_STEP_MS;
}
