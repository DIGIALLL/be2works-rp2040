# be2works-rp2040

Прошивка для **RP2040-Zero**, которая превращает плату в USB-SMBus адаптер, совместимый с **Silicon Labs CP2112**.
Компьютер видит устройство `VID 10C4 / PID EA90` «CP2112 HID USB-to-SMBus Bridge», поэтому с ним работают
программы, написанные под CP2112: **Be2Works (Battery EEPROM Works)**, примеры Silicon Labs на `SLABHIDtoSMBus.dll`,
драйвер Linux `hid-cp2112`.

> **English:** firmware that turns a ~$3 RP2040-Zero into a Silicon Labs CP2112 compatible USB-SMBus bridge for laptop
> battery tools such as Be2Works. Flash the UF2 from [Releases](../../releases/tag/latest), wire SDA→GP3, SCL→GP2,
> GND→battery minus, 4.7 kΩ pull-ups to 3V3. Tested with Be2Works 4.32 demo on a Lenovo/SANYO L12S4E01 battery.

**Проверено:** Be2Works 4.32 DEMO (Bohol) принимает плату как адаптер CP2112 и читает батарею Lenovo/SANYO
L12S4E01 (контроллер A2168): модель, производителя, ёмкости, циклы, напряжения банок, статус.
Запись в Data Flash через Be2Works пока не проверялась — перед записью сохрани резервную копию.

## Как прошить

1. Скачай `cp2112_emu-rp2040-zero.uf2` из [Releases → latest](../../releases/tag/latest)
   (или из артефактов последнего запуска во вкладке **Actions**).
2. Зажми кнопку **BOOT** на RP2040-Zero и подключи плату к ПК — появится диск `RPI-RP2`.
3. Перетащи `.uf2` на этот диск. Плата перезагрузится и определится как CP2112.
   Драйвер не нужен: Windows использует стандартный HID.

Проверка: в «Диспетчере устройств» → «Устройства HID» появится устройство с `VID_10C4&PID_EA90`,
или `python tools/cp2112_tool.py info`.

Обновлять прошивку можно без кнопки: `python tools/cp2112_tool.py bootloader` — появится диск `RPI-RP2`.

## Подключение к батарее

| RP2040-Zero | Батарея (SMBus) |
|---|---|
| **GP3** | SDA (Data) |
| **GP2** | SCL (Clock) |
| **GND** | GND / минус батареи |

- Поставь подтягивающие резисторы **4.7 кОм от SDA и от SCL к 3V3** платы. Встроенные подтяжки RP2040 включены,
  но они слабые (~50 кОм).
- **Не подавай на пины SDA/SCL больше 3.3 В.** Перед подключением измерь напряжение на SDA/SCL батареи относительно GND:
  если там больше 3.6 В, нужен преобразователь уровней (например, на BSS138).
- Плюс батареи к плате **не подключай**.
- Если на разъёме батареи 0 В, контроллер батареи выключен (защита от глубокого разряда или Permanent Failure)
  и по SMBus не отвечает. Разбудить его может только зарядное напряжение на силовых контактах
  (например, установка в ноутбук с зарядкой). Банки, разряженные ниже ~2 В, заряжать опасно.

Пины задаются в `src/config.h` (`SMBUS_SDA_PIN`, `SMBUS_SCL_PIN`) или при сборке:
`cmake -S . -B build -G Ninja -DCMAKE_C_FLAGS="-DSMBUS_SDA_PIN=4 -DSMBUS_SCL_PIN=5"`.
Если пара пинов подходит аппаратному I2C RP2040 (SDA на чётном, SCL на нечётном пине одного блока,
например SDA=GP4/SCL=GP5 или SDA=GP2/SCL=GP3), используется аппаратный контроллер, иначе — программный I2C.

### Lenovo L12S4E01 / L12L4A02 / L12M4A02 (7 контактов)

Проверено на L12S4E01: три средних контакта — SCL, SDA и ID, ID можно не подключать. Найди плюс и минус
мультиметром (12–16 В между крайними), подключи средние контакты к SDA/SCL и проверь командой `scan`:
если батарея не находится, поменяй провода SDA и SCL местами.

## Утилита проверки

`tools/cp2112_tool.py` — Python 3 для Windows, без сторонних пакетов:

```
python tools/cp2112_tool.py info        версия и настройки адаптера
python tools/cp2112_tool.py scan        поиск устройств на шине SMBus
python tools/cp2112_tool.py battery     основные регистры батареи (напряжение, ёмкость, циклы, банки...)
python tools/cp2112_tool.py log [сек]   какие запросы программа (например, Be2Works) шлёт адаптеру
python tools/cp2112_tool.py bootloader  перезагрузка платы в загрузчик UF2
```

Утилита работает параллельно с Be2Works, но запускай Be2Works первым: если адаптер в момент его старта
занят, Be2Works может написать «No adapter found».

## Что реализовано

Протокол по [AN495: CP2112 Interface Specification](https://www.silabs.com/documents/public/application-notes/an495-cp2112-interface-specification.pdf):

| Репорт | Назначение | Статус |
|---|---|---|
| 0x01 | Reset | ✅ перезагрузка платы; `0xB0` — в загрузчик UF2, `0xD0` — режим журнала |
| 0x02–0x04 | GPIO config / get / set | ⚪ значения хранятся, физических GPIO нет |
| 0x05 | Version (part 0x0C) | ✅ |
| 0x06 | SMBus config | ✅ частота 10–400 кГц, таймауты, auto send, повторы |
| 0x10 / 0x11 / 0x14 | Read / Write-Read / Write | ✅ |
| 0x12 / 0x13 | Force read / Read response | ✅ до 512 байт; и по interrupt IN, и по GET_REPORT |
| 0x15 / 0x16 | Transfer status | ✅ и по interrupt IN, и по GET_REPORT |
| 0x17 | Cancel | ✅ |
| 0x20–0x24 | Lock byte, USB config, строки | ⚪ принимаются и хранятся в RAM до переподключения, VID/PID не меняются |

Особенности, найденные при проверке с Be2Works:
- Be2Works забирает ответы не из interrupt IN, а запросом GET_REPORT (`HidD_GetInputReport`) — прошивка
  выполняет ожидающую транзакцию и отвечает на него.
- HID-дескриптор должен совпадать с CP2112: с любым лишним репортом Be2Works пишет «No adapter found».
  Поэтому журнал обмена отдаётся через репорт серийного номера 0x24 в режиме журнала (Reset `0xD0`),
  а не отдельным репортом.
- Программный I2C держит SDA 1 мкс после спада SCL (SMBus требует ≥ 300 нс), иначе контроллер батареи
  принимает смену SDA за START/STOP и не подтверждает адрес.

Ограничения:
- PEC (контрольная сумма SMBus) считается программой на ПК — адаптер передаёт байты как есть, как и настоящий CP2112.
- Таймаут 0 («без ограничения») заменяется на 1 с, повторов при NACK — не больше 3 (`src/config.h`).
- Режим SMBus-slave и multi-master не поддерживаются.

## Сборка вручную

Нужны `gcc-arm-none-eabi`, `cmake`, `ninja` и [pico-sdk](https://github.com/raspberrypi/pico-sdk) 2.1.x:

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja
cmake --build build
# результат: build/cp2112_emu.uf2
```

Тесты протокола на ПК: `make -C tests run`. Для другой RP2040-платы поменяй `PICO_BOARD` в `CMakeLists.txt`.

## Если не работает

1. `python tools/cp2112_tool.py info` — адаптер должен отвечать (part 0x0C).
2. `python tools/cp2112_tool.py scan` — батарея должна найтись по адресу 0x0B. Если нет: поменяй SDA/SCL,
   проверь общий GND и подтяжки, измерь напряжение на разъёме батареи (0 В — батарея выключена).
3. `python tools/cp2112_tool.py battery` — если читается, адаптер и подключение в порядке.
4. Если Be2Works всё равно не работает — запиши `python tools/cp2112_tool.py log 60`, пока нажимаешь кнопки
   в программе, и приложи журнал к issue: по нему видно, какие запросы программа шлёт и что ей не нравится.
