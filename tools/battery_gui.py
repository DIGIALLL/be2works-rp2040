"""Монитор батареи ноутбука (SBS, адрес 0x0B) через адаптер CP2112 / эту прошивку. Только Python 3, tkinter.

  python battery_gui.py

Только чтение: живые значения, графики, запись в CSV, дамп всех регистров 0x00-0xFF в JSON и сравнение двух дампов.
Ничего не пишет в батарею.
"""
import csv
import json
import os
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cp2112_tool as cp  # noqa: E402

ADDR = cp.BATTERY
BACKUP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backup")
POLL_S = 1.0
GRAPH_POINTS = 300

STATUS_BITS = [
    (0x8000, "OCA"), (0x4000, "TCA"), (0x1000, "TDA"), (0x0800, "RTA"), (0x0400, "RCA"),
    (0x0200, "?0200"), (0x0100, "?0100"), (0x0080, "INIT"), (0x0040, "DSG"),
    (0x0020, "FULL"), (0x0010, "EMPTY"),
]
STATUS_ERR = {0: "OK", 1: "BUSY", 2: "RESERVED", 3: "UNSUPPORTED", 4: "UNKNOWN", 5: "OVERRUN", 6: "UNDERRUN"}

# регистр -> (подпись, функция форматирования, единица)
def s16(v): return v - 65536 if v > 32767 else v
def date_s(v): return f"{1980 + (v >> 9)}-{(v >> 5) & 15:02d}-{v & 31:02d}"

FIELDS = [
    (0x09, "Напряжение", lambda v: f"{v}", "мВ"),
    (0x0A, "Ток", lambda v: f"{s16(v)}", "мА"),
    (0x08, "Температура", lambda v: f"{v / 10 - 273.15:.1f}", "°C"),
    (0x0D, "Заряд (RSOC)", lambda v: f"{v}", "%"),
    (0x0E, "Износ (ASOC)", lambda v: f"{v}", "%"),
    (0x0F, "Остаток", lambda v: f"{v}", "мА·ч"),
    (0x10, "Полная ёмкость", lambda v: f"{v}", "мА·ч"),
    (0x18, "Проектная ёмкость", lambda v: f"{v}", "мА·ч"),
    (0x19, "Проектное напряжение", lambda v: f"{v}", "мВ"),
    (0x17, "Циклы", lambda v: f"{v}", ""),
    (0x14, "Запрос тока зарядки", lambda v: f"{v}", "мА"),
    (0x15, "Запрос напряжения зарядки", lambda v: f"{v}", "мВ"),
    (0x12, "Время до разряда", lambda v: "—" if v == 0xFFFF else f"{v}", "мин"),
    (0x13, "Время до заряда", lambda v: "—" if v == 0xFFFF else f"{v}", "мин"),
    (0x1B, "Дата выпуска", date_s, ""),
    (0x1C, "Серийный №", lambda v: f"{v} (0x{v:04X})", ""),
]
CELL_REGS = (0x3C, 0x3D, 0x3E, 0x3F)


def decode_status(v):
    flags = [n for b, n in STATUS_BITS if v & b]
    return f"0x{v:04X}  " + (" | ".join(flags) if flags else "—") + f"  (код {STATUS_ERR.get(v & 15, v & 15)})"


class Worker(threading.Thread):
    """Единственный поток, который общается с адаптером."""

    def __init__(self, out_q):
        super().__init__(daemon=True)
        self.out = out_q
        self.cmds = queue.Queue()
        self.stop = False
        self.dev = None

    def run(self):
        try:
            self.dev = cp.CP2112()
        except BaseException as e:  # noqa: BLE001
            self.out.put(("error", f"Адаптер не найден: {e}"))
            return
        self.out.put(("connected", None))
        strings = {}
        next_poll = 0.0
        while not self.stop:
            try:
                cmd = self.cmds.get(timeout=0.05)
            except queue.Empty:
                cmd = None
            if cmd == "dump":
                self.dump()
            now = time.time()
            if now >= next_poll:
                next_poll = now + POLL_S
                self.poll(strings)

    def poll(self, strings):
        d = self.dev
        data = {}
        for reg, *_ in FIELDS:
            data[reg] = d.read_word(ADDR, reg)
        for reg in CELL_REGS:
            data[reg] = d.read_word(ADDR, reg)
        data[0x16] = d.read_word(ADDR, 0x16)
        if not strings:
            for reg in (0x20, 0x21, 0x22):
                s = d.read_block(ADDR, reg)
                if s is not None:
                    strings[reg] = s
        alive = data[0x09] is not None
        self.out.put(("data", (time.time(), data, dict(strings), alive)))

    def dump(self):
        d = self.dev
        out = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "words": {}, "blocks": {}}
        for r in range(0x100):
            v = d.read_word(ADDR, r)
            if v is not None:
                out["words"][f"0x{r:02X}"] = v
            if r % 16 == 15:
                self.out.put(("dump_progress", (r + 1) / 256))
        for r in (0x20, 0x21, 0x22, 0x23, 0x2F):
            s = d.read_block(ADDR, r)
            if s is not None:
                out["blocks"][f"0x{r:02X}"] = s
        self.out.put(("dump_done", out))


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Монитор батареи SBS (CP2112)")
        self.geometry("1000x720")
        self.q = queue.Queue()
        self.worker = Worker(self.q)
        self.history = []          # (t, mV, mA)
        self.csv_file = None
        self.csv_writer = None
        self.vars = {}
        self.build()
        self.worker.start()
        self.after(100, self.pump)
        self.protocol("WM_DELETE_WINDOW", self.close)

    def build(self):
        top = ttk.Frame(self)
        top.pack(fill="x", padx=8, pady=6)
        self.state = ttk.Label(top, text="Подключение…", font=("Segoe UI", 11, "bold"))
        self.state.pack(side="left")
        ttk.Button(top, text="Дамп регистров…", command=self.do_dump).pack(side="right")
        ttk.Button(top, text="Сравнить дампы…", command=self.do_compare).pack(side="right", padx=4)
        self.csv_btn = ttk.Button(top, text="Запись CSV", command=self.toggle_csv)
        self.csv_btn.pack(side="right", padx=4)

        self.ident = ttk.Label(self, text="", font=("Segoe UI", 10))
        self.ident.pack(anchor="w", padx=8)
        self.status = ttk.Label(self, text="", font=("Consolas", 10))
        self.status.pack(anchor="w", padx=8)

        mid = ttk.Frame(self)
        mid.pack(fill="x", padx=8, pady=6)
        grid = ttk.Frame(mid)
        grid.pack(side="left", anchor="n")
        for i, (reg, name, _f, unit) in enumerate(FIELDS):
            ttk.Label(grid, text=name).grid(row=i % 8, column=(i // 8) * 3, sticky="w", padx=(0, 8))
            v = tk.StringVar(value="—")
            self.vars[reg] = v
            ttk.Label(grid, textvariable=v, width=14, anchor="e", font=("Consolas", 10, "bold")).grid(
                row=i % 8, column=(i // 8) * 3 + 1)
            ttk.Label(grid, text=unit, width=5).grid(row=i % 8, column=(i // 8) * 3 + 2, padx=(0, 24))

        cells = ttk.LabelFrame(mid, text="Банки, мВ")
        cells.pack(side="left", padx=20, anchor="n")
        self.cell_bars = []
        self.cell_lbl = []
        for i in range(4):
            ttk.Label(cells, text=f"{i + 1}").grid(row=i, column=0)
            b = ttk.Progressbar(cells, length=180, maximum=1200, mode="determinate")
            b.grid(row=i, column=1, padx=4, pady=2)
            l = ttk.Label(cells, text="—", width=6, font=("Consolas", 10, "bold"))
            l.grid(row=i, column=2)
            self.cell_bars.append(b)
            self.cell_lbl.append(l)
        self.spread = ttk.Label(cells, text="Разброс: —")
        self.spread.grid(row=4, column=0, columnspan=3, pady=4)

        self.prog = ttk.Progressbar(self, mode="determinate", maximum=1)
        self.prog.pack(fill="x", padx=8)
        self.canvas = tk.Canvas(self, bg="#111", highlightthickness=0, height=300)
        self.canvas.pack(fill="both", expand=True, padx=8, pady=8)
        self.canvas.bind("<Configure>", lambda e: self.draw())

    # ---- события из потока ----
    def pump(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                getattr(self, "on_" + kind)(payload)
        except queue.Empty:
            pass
        self.after(100, self.pump)

    def on_error(self, msg):
        self.state.config(text=msg, foreground="red")

    def on_connected(self, _):
        self.state.config(text="Адаптер подключён", foreground="green")

    def on_data(self, payload):
        t, data, strings, alive = payload
        if not alive:
            self.state.config(text="Батарея не отвечает (0x0B)", foreground="red")
            return
        self.state.config(text="Батарея отвечает", foreground="green")
        for reg, _n, fmt, _u in FIELDS:
            v = data.get(reg)
            self.vars[reg].set("—" if v is None else fmt(v))
        ids = [strings.get(r) for r in (0x20, 0x21, 0x22) if strings.get(r)]
        self.ident.config(text="  ".join(ids))
        st = data.get(0x16)
        self.status.config(text="BatteryStatus: " + ("—" if st is None else decode_status(st)))
        cells = [data.get(r) for r in CELL_REGS]
        good = [c for c in cells if c]
        for i, c in enumerate(cells):
            self.cell_lbl[i].config(text="—" if not c else str(c))
            self.cell_bars[i]["value"] = max(0, (c or 3000) - 3000)
        if len(good) == 4:
            self.spread.config(text=f"Разброс: {max(good) - min(good)} мВ, сумма {sum(good)} мВ")
        mv, ma = data.get(0x09), data.get(0x0A)
        self.history.append((t, mv, None if ma is None else s16(ma)))
        del self.history[:-GRAPH_POINTS]
        if self.csv_writer:
            row = [time.strftime("%H:%M:%S", time.localtime(t))] + [data.get(r) for r, *_ in FIELDS] + cells
            self.csv_writer.writerow(row)
            self.csv_file.flush()
        self.draw()

    def on_dump_progress(self, frac):
        self.prog["value"] = frac

    def on_dump_done(self, out):
        self.prog["value"] = 0
        os.makedirs(BACKUP_DIR, exist_ok=True)
        fn = os.path.join(BACKUP_DIR, f"sbs-dump-{time.strftime('%Y%m%d-%H%M%S')}.json")
        with open(fn, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=1)
        messagebox.showinfo("Дамп", f"Сохранено: {fn}\nОтветили регистров: {len(out['words'])}")

    # ---- действия ----
    def do_dump(self):
        self.worker.cmds.put("dump")

    def do_compare(self):
        a = filedialog.askopenfilename(title="Первый дамп (до)", initialdir=BACKUP_DIR, filetypes=[("JSON", "*.json")])
        b = a and filedialog.askopenfilename(title="Второй дамп (после)", initialdir=BACKUP_DIR,
                                              filetypes=[("JSON", "*.json")])
        if not a or not b:
            return
        wa, wb = (json.load(open(p, encoding="utf-8"))["words"] for p in (a, b))
        lines = []
        for k in sorted(set(wa) | set(wb), key=lambda x: int(x, 16)):
            va, vb = wa.get(k), wb.get(k)
            if va != vb:
                f = lambda v: "—" if v is None else f"{v} (0x{v:04X})"
                lines.append(f"{k}: {f(va)} -> {f(vb)}")
        win = tk.Toplevel(self)
        win.title("Различия дампов")
        txt = tk.Text(win, width=70, height=30, font=("Consolas", 10))
        txt.pack(fill="both", expand=True)
        txt.insert("1.0", "\n".join(lines) if lines else "Дампы совпадают")

    def toggle_csv(self):
        if self.csv_file:
            self.csv_file.close()
            self.csv_file = self.csv_writer = None
            self.csv_btn.config(text="Запись CSV")
            return
        os.makedirs(BACKUP_DIR, exist_ok=True)
        fn = os.path.join(BACKUP_DIR, f"battery-log-{time.strftime('%Y%m%d-%H%M%S')}.csv")
        self.csv_file = open(fn, "w", newline="", encoding="utf-8")
        self.csv_writer = csv.writer(self.csv_file)
        self.csv_writer.writerow(["time"] + [n for _r, n, *_ in FIELDS] + [f"cell{i + 1}_mV" for i in range(4)])
        self.csv_btn.config(text="Стоп CSV")
        self.state.config(text=f"Пишу {os.path.basename(fn)}")

    def draw(self):
        c = self.canvas
        c.delete("all")
        w, h = c.winfo_width(), c.winfo_height()
        pts = [p for p in self.history if p[1] is not None]
        if len(pts) < 2 or w < 100:
            return
        t0, t1 = pts[0][0], pts[-1][0]
        span = max(t1 - t0, 1)

        def plot(vals, color, label, x0):
            ys = [v for _t, v in vals]
            lo, hi = min(ys), max(ys)
            if hi == lo:
                lo, hi = lo - 1, hi + 1
            xy = []
            for t, v in vals:
                xy += [40 + (t - t0) / span * (w - 80), 15 + (1 - (v - lo) / (hi - lo)) * (h - 40)]
            c.create_line(*xy, fill=color, width=2)
            c.create_text(x0, 10, anchor="w", fill=color, text=f"{label}: {lo}…{hi}")

        plot([(t, v) for t, v, _a in pts], "#4da3ff", "Напряжение, мВ", 40)
        cur = [(t, a) for t, _v, a in pts if a is not None]
        if len(cur) > 1:
            plot(cur, "#ffb84d", "Ток, мА", 260)
        c.create_text(w - 40, h - 8, anchor="e", fill="#888", text=f"{int(span)} с")

    def close(self):
        self.worker.stop = True
        if self.csv_file:
            self.csv_file.close()
        self.destroy()
        os._exit(0)  # поток чтения HID висит в ReadFile


if __name__ == "__main__":
    App().mainloop()
