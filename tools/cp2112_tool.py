"""Проверка адаптера CP2112 (и этой прошивки) на Windows: только Python 3, без сторонних пакетов.

  python cp2112_tool.py info        версия и настройки адаптера
  python cp2112_tool.py scan        поиск устройств на шине SMBus
  python cp2112_tool.py battery     чтение основных регистров батареи (адрес 0x0B)
  python cp2112_tool.py log [сек]   журнал запросов программы к адаптеру (только эта прошивка)
  python cp2112_tool.py autodetect  перебор ролей SDA/SCL/GND на GP2–GP4, пока не ответит батарея
  python cp2112_tool.py bootloader  перезагрузить плату в загрузчик UF2 (только эта прошивка)
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import threading
import time

VID_PID = "vid_10c4&pid_ea90"
BATTERY = 0x0B

setupapi = ctypes.WinDLL("setupapi")
hid = ctypes.WinDLL("hid")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wt.HANDLE
setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p


class GUID(ctypes.Structure):
    _fields_ = [("d1", wt.DWORD), ("d2", wt.WORD), ("d3", wt.WORD), ("d4", ctypes.c_ubyte * 8)]


class IFDATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("guid", GUID), ("flags", wt.DWORD), ("rsv", ctypes.c_void_p)]


class CAPS(ctypes.Structure):
    _fields_ = [("Usage", wt.USHORT), ("UsagePage", wt.USHORT), ("In", wt.USHORT), ("Out", wt.USHORT),
                ("Feat", wt.USHORT), ("rest", wt.USHORT * 17)]


def find_path():
    g = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(g))
    devs = ctypes.c_void_p(setupapi.SetupDiGetClassDevsW(ctypes.byref(g), None, None, 0x12))
    i = 0
    while True:
        d = IFDATA()
        d.cbSize = ctypes.sizeof(d)
        if not setupapi.SetupDiEnumDeviceInterfaces(devs, None, ctypes.byref(g), i, ctypes.byref(d)):
            return None
        need = wt.DWORD()
        setupapi.SetupDiGetDeviceInterfaceDetailW(devs, ctypes.byref(d), None, 0, ctypes.byref(need), None)
        buf = ctypes.create_string_buffer(need.value)
        ctypes.cast(buf, ctypes.POINTER(wt.DWORD))[0] = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
        setupapi.SetupDiGetDeviceInterfaceDetailW(devs, ctypes.byref(d), buf, need, None, None)
        path = ctypes.wstring_at(ctypes.addressof(buf) + 4)
        if VID_PID in path.lower():
            return path
        i += 1


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


class CP2112:
    def __init__(self):
        path = find_path()
        if not path:
            sys.exit("Адаптер CP2112 (10C4:EA90) не найден")
        # Чтение и запись — через разные handle: синхронный ввод-вывод на одном handle
        # выполняется по очереди, и висящий ReadFile заблокировал бы WriteFile
        self.dev = self._open(path)
        self.rdev = self._open(path)
        pp = ctypes.c_void_p()
        hid.HidD_GetPreparsedData(self.dev, ctypes.byref(pp))
        self.caps = CAPS()
        hid.HidP_GetCaps(pp, ctypes.byref(self.caps))
        self.rx = []
        self.reader = None

    @staticmethod
    def _open(path):
        h = k32.CreateFileW(path, 0xC0000000, 3, None, 3, 0, None)
        if h == wt.HANDLE(-1).value:
            sys.exit(f"Не удалось открыть адаптер (ошибка {ctypes.get_last_error()}): "
                     "возможно, его занимает другая программа")
        return h

    def _reader(self):
        while True:
            b = (ctypes.c_ubyte * self.caps.In)()
            n = wt.DWORD()
            if not k32.ReadFile(self.rdev, b, self.caps.In, ctypes.byref(n), None):
                return
            self.rx.append(bytes(b)[: n.value])

    def string(self, fn):
        s = ctypes.create_unicode_buffer(128)
        return s.value if fn(self.dev, s, 256) else ""

    def get_feature(self, rid, n):
        b = (ctypes.c_ubyte * self.caps.Feat)()
        b[0] = rid
        return bytes(b)[1: n + 1] if hid.HidD_GetFeature(self.dev, b, self.caps.Feat) else None

    def set_feature(self, rid, *data):
        b = (ctypes.c_ubyte * self.caps.Feat)(rid, *data)
        return bool(hid.HidD_SetFeature(self.dev, b, self.caps.Feat))

    def send(self, *data):
        if not self.reader:
            self.reader = threading.Thread(target=self._reader, daemon=True)
            self.reader.start()
        b = (ctypes.c_ubyte * self.caps.Out)(*data)
        n = wt.DWORD()
        k32.WriteFile(self.dev, b, self.caps.Out, ctypes.byref(n), None)

    def wait(self, rid, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            for r in list(self.rx):
                if r[0] == rid:
                    self.rx.remove(r)
                    return r
            time.sleep(0.005)
        return None

    def status(self):
        # 0x16: [id, status0, status1, ...]; status0: 0 idle, 1 busy, 2 complete, 3 error
        for _ in range(100):
            self.send(0x15, 0x01)
            r = self.wait(0x16)
            if not r:
                return None
            if r[1] != 1:
                return r
            time.sleep(0.01)
        return None

    def read_word(self, addr7, cmd):
        self.send(0x11, addr7 << 1, 0x00, 0x02, 0x01, cmd)
        r = self.status()
        if not r or r[1] != 2:
            return None
        self.send(0x12, 0x00, 0x02)
        d = self.wait(0x13)
        return int.from_bytes(d[3:5], "little") if d and d[2] >= 2 else None

    def read_block(self, addr7, cmd):
        self.send(0x11, addr7 << 1, 0x00, 0x21, 0x01, cmd)
        r = self.status()
        if not r or r[1] != 2:
            return None
        self.send(0x12, 0x00, 0x21)
        d = self.wait(0x13)
        if not d:
            return None
        n = min(d[3], d[2] - 1)
        return bytes(d[4:4 + n]).decode("ascii", "replace")

    def probe(self, addr7):
        self.send(0x10, addr7 << 1, 0x00, 0x01)
        r = self.status()
        self.send(0x17, 0x01)
        return bool(r and r[1] == 2)


def cmd_info(a):
    print("Производитель:", a.string(hid.HidD_GetManufacturerString))
    print("Устройство:   ", a.string(hid.HidD_GetProductString))
    print("Серийный №:   ", a.string(hid.HidD_GetSerialNumberString))
    v = a.get_feature(0x05, 2)
    print("Версия:        part 0x%02X, ver %d" % (v[0], v[1]) if v else "Версия: нет ответа")
    c = a.get_feature(0x06, 13)
    if c:
        print(f"SMBus:         {int.from_bytes(c[0:4], 'big')} Гц, auto send {c[5]}, "
              f"таймауты {int.from_bytes(c[6:8], 'big')}/{int.from_bytes(c[8:10], 'big')} мс, "
              f"повторы {int.from_bytes(c[11:13], 'big')}")


def cmd_scan(a):
    found = [x for x in range(0x08, 0x78) if a.probe(x)]
    print("Устройства (7-битный адрес):", " ".join(f"0x{x:02X}" for x in found) or "нет")
    if not found:
        print("Проверь SDA/SCL, общий GND, подтяжки 4.7 кОм к 3V3 и что батарея не спит (0 В на разъёме)")


def cmd_battery(a):
    def signed(v):
        return v - 65536 if v > 32767 else v
    regs = [
        (0x09, "Напряжение", lambda v: f"{v} мВ"),
        (0x0A, "Ток", lambda v: f"{signed(v)} мА"),
        (0x08, "Температура", lambda v: f"{v / 10 - 273.15:.1f} °C"),
        (0x0D, "Заряд", lambda v: f"{v} %"),
        (0x0F, "Остаток", lambda v: f"{v} мА·ч"),
        (0x10, "Полная ёмкость", lambda v: f"{v} мА·ч"),
        (0x18, "Проектная ёмкость", lambda v: f"{v} мА·ч"),
        (0x19, "Проектное напряжение", lambda v: f"{v} мВ"),
        (0x17, "Циклы", str),
        (0x16, "BatteryStatus", lambda v: f"0x{v:04X}"),
        (0x1B, "Дата выпуска", lambda v: f"{1980 + (v >> 9)}-{(v >> 5) & 15:02}-{v & 31:02}"),
        (0x1C, "Серийный №", str),
        (0x3F, "Банка 1", lambda v: f"{v} мВ"),
        (0x3E, "Банка 2", lambda v: f"{v} мВ"),
        (0x3D, "Банка 3", lambda v: f"{v} мВ"),
        (0x3C, "Банка 4", lambda v: f"{v} мВ"),
    ]
    if a.read_word(BATTERY, 0x09) is None:
        sys.exit("Батарея (адрес 0x0B) не отвечает — запусти scan")
    for cmd, name, fmt in regs:
        v = a.read_word(BATTERY, cmd)
        print(f"{name:22} {fmt(v) if v is not None else '—'}")
    for cmd, name in ((0x20, "Производитель"), (0x21, "Модель"), (0x22, "Химия")):
        print(f"{name:22} {a.read_block(BATTERY, cmd) or '—'}")
    print("Регистры банок 0x3C–0x3F зависят от контроллера; сумма банок должна совпасть с напряжением")


# Варианты прошивки (Reset 0xE0 + n): пины-кандидаты GP2, GP3, GP4; пары (SDA, SCL) по индексам
# кандидатов, третий пин отпущен (0–5) или прижат к GND (6–11). Должно совпадать с src/smbus.c
PIN_CANDIDATES = (2, 3, 4)
VARIANT_PAIRS = ((1, 0), (0, 1), (2, 0), (0, 2), (2, 1), (1, 2))


def describe_variant(n):
    sda, scl = VARIANT_PAIRS[n % 6]
    other = 3 - sda - scl
    third = "к GND" if n >= 6 else "отпущен"
    return (f"SDA=GP{PIN_CANDIDATES[sda]}, SCL=GP{PIN_CANDIDATES[scl]}, "
            f"GP{PIN_CANDIDATES[other]} {third}")


def cmd_autodetect(a):
    # На медленной шине сработают и внутренние подтяжки (~50 кОм) на пине без внешнего резистора
    c = a.get_feature(0x06, 13)
    if c:
        a.set_feature(0x06, *((20000).to_bytes(4, "big") + c[4:]))
    print("Перебор ролей пинов GP2–GP4 по кругу, Ctrl+C — стоп. На плюс батареи пины не подключать!")
    rnd = 0
    while True:
        rnd += 1
        for n in range(len(VARIANT_PAIRS) * 2):
            a.set_feature(0x01, 0xE0 + n)
            time.sleep(0.02)
            if not a.probe(BATTERY):
                continue
            # Прижатая шина «отвечает» на любой адрес: пустой адрес должен молчать,
            # а напряжение батареи — быть правдоподобным и одинаковым два раза подряд
            v1 = a.read_word(BATTERY, 0x09)
            v2 = a.read_word(BATTERY, 0x09)
            if a.probe(0x75):
                print(f"[{time.strftime('%H:%M:%S')}] вариант {n:2}: {describe_variant(n)} — шина прижата")
                continue
            if v1 is None or v2 is None or not 2000 <= v1 <= 25000 or abs(v1 - v2) > 200:
                print(f"[{time.strftime('%H:%M:%S')}] вариант {n:2}: {describe_variant(n)} — "
                      f"ACK на 0x0B, но напряжение {v1}/{v2} — похоже на помеху")
                continue
            print(f"\nНАЙДЕНО, вариант {n}: {describe_variant(n)}\n")
            cmd_battery(a)
            print(f"\nПлата остаётся в варианте {n} до переподключения USB")
            return
        if rnd % 10 == 0:
            print(f"[{time.strftime('%H:%M:%S')}] круг {rnd}: батарея не ответила ни в одном из 12 вариантов")


def cmd_log(a, secs):
    # Reset 0xD0 переключает режим журнала: репорт серийного номера 0x24 отдаёт записи журнала.
    # Если 0x24 ещё возвращает строку ([длина, 0x03, ...]) — включаем режим
    # В записи журнала r[0] (число записей) не больше 7, у строки r[0] — длина дескриптора
    def is_string(r):
        return r[1] == 0x03 and r[0] > 7
    r = a.get_feature(0x24, 63)
    if r and is_string(r):
        a.set_feature(0x01, 0xD0)
    # Записи копятся и без режима журнала — старые пропускаем
    while (r := a.get_feature(0x24, 63)) and not is_string(r) and r[0]:
        pass
    kinds = {"S": "set feature", "G": "get feature", "O": "output", "I": "get input"}
    print(f"Журнал {secs:g} с. S/G — feature-репорты, O — запросы, I — GET_REPORT входных репортов")
    end = time.time() + secs
    while time.time() < end:
        r = a.get_feature(0x24, 63)
        if not r or is_string(r):
            sys.exit("Журнал не поддерживается: на плате другая прошивка")
        if r[1]:
            print(f"... потеряно записей: {r[1]}")
        for i in range(r[0]):
            e = r[2 + 8 * i: 10 + 8 * i]
            kind = chr(e[0])
            print(f"{kind} 0x{e[1]:02X} len {e[2]:3} | {hexs(e[3:3 + min(5, e[2])]):14} ({kinds.get(kind, '?')})")
        if r[0] == 0:
            time.sleep(0.05)
    a.set_feature(0x01, 0xD0)   # обратно: 0x24 снова отдаёт серийный номер


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    cmd = sys.argv[1] if len(sys.argv) > 1 else "info"
    a = CP2112()
    if cmd == "info":
        cmd_info(a)
    elif cmd == "scan":
        cmd_scan(a)
    elif cmd == "battery":
        cmd_battery(a)
    elif cmd == "log":
        cmd_log(a, float(sys.argv[2]) if len(sys.argv) > 2 else 30)
    elif cmd == "autodetect":
        cmd_autodetect(a)
    elif cmd == "bootloader":
        print("Перезагрузка в загрузчик:", "OK" if a.set_feature(0x01, 0xB0) else "ошибка")
    else:
        print(__doc__)
    # Поток чтения висит в ReadFile; обычное завершение Python при этом может упасть
    sys.stdout.flush()
    os._exit(0)


if __name__ == "__main__":
    main()
