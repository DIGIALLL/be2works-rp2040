#ifndef AUTOSCAN_H
#define AUTOSCAN_H

#include <stdbool.h>
#include <stdint.h>

// Автономный поиск контактов батареи внутри прошивки: пока компьютер не работает с шиной,
// плата сама перебирает роли пинов GP2–GP4 и ищет ответ SMBus-батареи (адрес 0x0B).
// Ничего подключать к ПК не нужно: нашла — остаётся на этих пинах и зажигает зелёный светодиод.

typedef enum {
    AUTOSCAN_OFF,        // выключен: ручной выбор пинов, аппаратный I2C или команда хоста
    AUTOSCAN_SEARCHING,  // батарея не найдена, идёт перебор
    AUTOSCAN_FOUND,      // батарея отвечает, пины зафиксированы
} autoscan_state_t;

// Главный цикл; host_hz — частота шины, заданная хостом (возвращаем её после поиска)
void autoscan_task(uint32_t host_hz);

// Хост начал / закончил транзакцию. Перед ней поиск возвращает пины по умолчанию
void autoscan_host_transfer_begin(void);
void autoscan_host_transfer_end(uint8_t addr, bool ok);

// Хост сам выбрал пины (Reset 0xE0+n): поиск отключается до переподключения или rescan
void autoscan_manual_override(void);
// Начать поиск заново (Reset 0xC0)
void autoscan_restart(void);

autoscan_state_t autoscan_state(void);
bool autoscan_battery_ok(void);

#endif
