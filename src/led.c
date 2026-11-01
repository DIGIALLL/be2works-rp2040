// Статусный светодиод WS2812 на RP2040-Zero (GP16) через PIO.

#include "led.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

static PIO led_pio = pio0;
static uint led_sm;
static bool led_ready;
static uint32_t last_grb = 0xFFFFFFFFu;
static uint32_t flash_until_ms;
static uint32_t flash_grb;

static uint32_t grb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
}

static void led_put(uint32_t color) {
    if (!led_ready || color == last_grb) return;
    last_grb = color;
    pio_sm_put_blocking(led_pio, led_sm, color << 8u);
}

void led_init(void) {
    int sm = pio_claim_unused_sm(led_pio, false);
    if (sm < 0) return;
    led_sm = (uint)sm;
    uint offset = pio_add_program(led_pio, &ws2812_program);
    ws2812_program_init(led_pio, led_sm, offset, LED_PIN, 800000);
    led_ready = true;
    led_put(0);
}

void led_flash(bool ok) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    flash_grb = ok ? grb(0, 0, LED_BRIGHTNESS) : grb(LED_BRIGHTNESS, 0, 0);
    flash_until_ms = now + (ok ? 40 : 120);
}

void led_task(led_status_t status) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if ((int32_t)(flash_until_ms - now) > 0) {
        led_put(flash_grb);
        return;
    }

    const uint8_t v = LED_BRIGHTNESS;
    uint32_t color = 0;
    switch (status) {
    case LED_NO_USB:  color = (now % 1000) < 150 ? grb(v, 0, 0) : 0; break;
    case LED_IDLE:    color = grb(v, v / 2, 0); break;
    case LED_SEARCH:  color = (now % 400) < 200 ? grb(v, 0, v) : 0; break;
    case LED_BATTERY: color = grb(0, v, 0); break;
    }
    led_put(color);
}
