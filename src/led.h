#ifndef LED_H
#define LED_H

#include <stdbool.h>

// Состояние, которое показывает светодиод
typedef enum {
    LED_NO_USB,     // красный мигает: компьютер не настроил USB
    LED_IDLE,       // жёлтый: USB есть, батареи нет, поиск не идёт
    LED_SEARCH,     // фиолетовый мигает: прошивка перебирает пины в поисках батареи
    LED_BATTERY,    // зелёный: батарея отвечает
} led_status_t;

void led_init(void);
// Вызывать в главном цикле; сама решает, когда обновлять светодиод
void led_task(led_status_t status);
// Короткая вспышка на обмен по SMBus: синяя — успех, красная — ошибка
void led_flash(bool ok);

#endif
