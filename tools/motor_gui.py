#!/usr/bin/env python3
"""
BLDC Motor Kontrol Arayüzü — Tkinter GUI
==========================================
Mouse ile PWM kontrolü, WASD+/- eşzamanlı destek,
Ayarlar paneli, Identify, Brake (pasif placeholder).

Firmware'da "mode control" aktifken çalışır.

Kullanım:
    python3 motor_gui.py                    # varsayılan /dev/ttyUSB0
    python3 motor_gui.py /dev/ttyACM0       # port belirt
    python3 motor_gui.py /dev/ttyUSB0 115200 # port + baud
"""

import sys
import time
import threading

try:
    import tkinter as tk
    from tkinter import scrolledtext
except ImportError:
    print("tkinter gerekli: sudo apt install python3-tk")
    sys.exit(1)

try:
    import serial
except ImportError:
    print("pyserial gerekli: pip install pyserial")
    sys.exit(1)

# ─── Sabitler ─────────────────────────────────────────────────────
DEFAULT_PORT = "/dev/ttyUSB1"
DEFAULT_BAUD = 115200
PWM_STEP = 10
HEARTBEAT_INTERVAL = 0.5  # sn — heartbeat aralığı
PWM_THROTTLE_MS = 50  # sn — slider seri komut throttle
RPM_MAX_DISPLAY = 700

# Catppuccin Mocha renkleri
C = {
    "bg": "#1e1e2e",
    "fg": "#cdd6f4",
    "green": "#a6e3a1",
    "red": "#f38ba8",
    "yellow": "#f9e2af",
    "cyan": "#89dceb",
    "magenta": "#f5c2e7",
    "blue": "#89b4fa",
    "surface": "#313244",
    "overlay": "#45475a",
    "dim": "#585b70",
}


# ═══════════════════════════════════════════════════════════════════
#  MotorController — Seri haberleşme + telemetri + heartbeat
# ═══════════════════════════════════════════════════════════════════
class MotorController:
    """Seri port üzerinden motor ile iletişim kurar.
    Ayrı thread'lerde telemetri okur ve heartbeat gönderir."""

    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        time.sleep(0.1)

        # Telemetri durumu (UI tick tarafından okunur)
        self.rpm = 0
        self.duty = 0
        self.pwm_set = 150  # Host tarafından hedeflenen PWM
        self.pwm_act = 0  # Firmware'ın aktif duty'si
        self.direction = 0  # 0=durdu 1=ileri -1=geri
        self.fw_direction = 0
        self.dir_str = "-"
        self.phase = 0
        self.phase_name = "STOPPED"
        self.hall_raw = 0

        # Mod takibi
        self.running = True
        self.mode_confirmed = False
        self.current_mode = "NORMAL"  # "NORMAL" | "CONTROL" | "SETTINGS"

        # Hareket state — heartbeat bunu okur
        self.movement = None  # None | "f" | "b"

        self._send_lock = threading.Lock()

        # İstatistik
        self.cmd_count = 0
        self.telem_count = 0
        self.last_response = ""
        self.error_msg = ""

        # Settings penceresine log yazma referansı
        self.settings_log = None

        # Thread-safe log queue (settings penceresi için)
        self._log_queue = []
        self._log_lock = threading.Lock()

        self.ser.reset_input_buffer()

    # ── Komut gönderimi ───────────────────────────────────────────
    def send_command(self, cmd):
        try:
            with self._send_lock:
                self.ser.write(f"{cmd}\n".encode("utf-8"))
            self.cmd_count += 1
        except serial.SerialException:
            pass

    # ── Mod yönetimi ──────────────────────────────────────────────
    def activate_control_mode(self):
        self.send_command("mode control")
        time.sleep(0.3)
        try:
            while self.ser.in_waiting > 0:
                line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                if "[MODE] CONTROL" in line or "Mode=CONTROL" in line:
                    self.mode_confirmed = True
                    self.current_mode = "CONTROL"
        except Exception:
            pass

    def restore_normal_mode(self):
        self.send_command("s")
        time.sleep(0.05)
        self.send_command("mode normal")
        time.sleep(0.1)

    # ── Hareket komutları ─────────────────────────────────────────
    def forward(self):
        self.direction = 1
        self.movement = "f"
        self.send_command("f")

    def backward(self):
        self.direction = -1
        self.movement = "b"
        self.send_command("b")

    def stop(self):
        self.direction = 0
        self.movement = None
        self.send_command("s")

    # ── PWM ───────────────────────────────────────────────────────
    def set_pwm(self, val):
        val = max(0, min(255, val))
        self.pwm_set = val
        self.send_command(f"pwm {val}")

    def pwm_up(self):
        self.pwm_set = min(255, self.pwm_set + PWM_STEP)
        self.send_command(f"pwm {self.pwm_set}")

    def pwm_down(self):
        self.pwm_set = max(0, self.pwm_set - PWM_STEP)
        self.send_command(f"pwm {self.pwm_set}")

    # ── Heartbeat — FIX #6: Sadece CONTROL modunda çalışır ────────
    def _heartbeat_loop(self):
        """Periyodik olarak f/b komutu gönderir (lease yenileme).
        Sadece CONTROL modunda, mode_confirmed=True ve movement aktifken çalışır."""
        while self.running:
            if (
                self.current_mode == "CONTROL"
                and self.mode_confirmed
                and self.movement in ("f", "b")
            ):
                self.send_command(self.movement)
                time.sleep(HEARTBEAT_INTERVAL)
            else:
                time.sleep(0.03)

    # ── Telemetri okuma ───────────────────────────────────────────
    def read_telemetry_loop(self):
        while self.running:
            try:
                if self.ser.in_waiting > 0:
                    line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                    if not line:
                        continue
                    if line.startswith("RPM:"):
                        self._parse_telemetry(line)
                        self.telem_count += 1
                    elif line.startswith("PWM:"):
                        try:
                            self.pwm_set = int(line.split(":")[1])
                        except (ValueError, IndexError):
                            pass
                    elif line.startswith("OK:"):
                        self._parse_ok(line)
                    elif line.startswith("[MODE]"):
                        self._handle_mode_response(line)
                    else:
                        self.last_response = line
                        # Thread-safe: settings loga yazma yerine queue'ya at
                        with self._log_lock:
                            self._log_queue.append(line)
                else:
                    time.sleep(0.01)
            except (serial.SerialException, OSError):
                self.error_msg = "Serial bağlantı hatası!"
                time.sleep(0.1)
            except Exception:
                time.sleep(0.01)

    def _handle_mode_response(self, line):
        if "CONTROL" in line:
            self.mode_confirmed = True
            self.current_mode = "CONTROL"
        elif "SETTINGS" in line:
            self.mode_confirmed = False
            self.current_mode = "SETTINGS"
        elif "NORMAL" in line:
            self.mode_confirmed = False
            self.current_mode = "NORMAL"

    def _parse_ok(self, line):
        try:
            for part in line.split(","):
                if ":" not in part:
                    continue
                key, val = part.split(":", 1)
                if key == "PWM":
                    self.pwm_set = int(val)
                elif key == "OK":
                    if val == "FWD":
                        self.direction = 1
                    elif val == "REV":
                        self.direction = -1
                    elif val == "STOP":
                        self.direction = 0
                    elif val == "BRAKE":
                        self.direction = 0
        except (ValueError, IndexError):
            pass

    def _parse_telemetry(self, line):
        try:
            for part in line.split(","):
                if ":" not in part:
                    continue
                key, val = part.split(":", 1)
                if key == "RPM":
                    self.rpm = int(val)
                elif key == "D":
                    self.duty = int(val)
                elif key == "DIR":
                    self.dir_str = val
                    if val == "F":
                        self.direction = 1
                    elif val == "R":
                        self.direction = -1
                    else:
                        self.direction = 0
                elif key == "PH":
                    ph = int(val)
                    self.phase = ph
                    phases = [
                        "STOPPED",
                        "KICK",
                        "RUNNING",
                        "NEUTRAL_WAIT",
                        "FAULT",
                        "BRAKE",
                    ]
                    self.phase_name = phases[ph] if ph < len(phases) else "?"
                elif key == "PWM_SET":
                    self.pwm_set = int(val)
                elif key == "PWM_ACT":
                    self.pwm_act = int(val)
                elif key == "PWM":
                    if "PWM_SET" not in line:
                        self.pwm_set = int(val)
                elif key == "PDIR":
                    self.fw_direction = int(val)
                    self.direction = self.fw_direction
                elif key == "H":
                    self.hall_raw = int(val)
        except (ValueError, IndexError):
            pass

    # ── Temiz kapanış ─────────────────────────────────────────────
    def close(self):
        self.movement = None
        self.running = False
        try:
            self.restore_normal_mode()
            time.sleep(0.1)
            self.ser.close()
        except Exception:
            pass


# ═══════════════════════════════════════════════════════════════════
#  SettingsWindow — Firmware ayarları (Settings modu)
# ═══════════════════════════════════════════════════════════════════
class SettingsWindow(tk.Toplevel):
    """Açılır ayar penceresi. Açılınca 'mode settings',
    kapanınca 'mode control' gönderir."""

    def __init__(self, parent, ctrl):
        super().__init__(parent)
        self.ctrl = ctrl
        self.parent = parent

        self.title("Motor Ayarları")
        self.geometry("440x750")
        self.configure(bg=C["bg"])
        self.resizable(False, False)

        self.protocol("WM_DELETE_WINDOW", self._on_close)

        # FIX #1: Settings moduna geç — motor zaten _safe_stop ile durduruldu
        self.ctrl.send_command("mode settings")
        time.sleep(0.1)

        self._build_ui()

        # Serial log referansını controller'a ver
        self.ctrl.settings_log = self.log_text

    def _send(self, cmd):
        self.ctrl.send_command(cmd)

    # ── UI Builder helpers ────────────────────────────────────────
    def _section(self, text, row):
        tk.Label(
            self,
            text=text,
            bg=C["bg"],
            fg=C["cyan"],
            font=("monospace", 11, "bold"),
            anchor="w",
        ).grid(row=row, column=0, columnspan=3, sticky="w", padx=15, pady=(10, 2))

    def _toggle_row(self, label, cmd_on, cmd_off, row):
        fr = tk.Frame(self, bg=C["bg"])
        fr.grid(row=row, column=0, columnspan=3, sticky="w", padx=15, pady=2)
        tk.Label(
            fr,
            text=label,
            bg=C["bg"],
            fg=C["fg"],
            font=("monospace", 10),
            width=12,
            anchor="w",
        ).pack(side="left")
        tk.Button(
            fr,
            text="ON",
            command=lambda: self._send(cmd_on),
            bg=C["green"],
            fg=C["bg"],
            font=("monospace", 9, "bold"),
            relief="flat",
            width=5,
            cursor="hand2",
        ).pack(side="left", padx=3)
        tk.Button(
            fr,
            text="OFF",
            command=lambda: self._send(cmd_off),
            bg=C["red"],
            fg=C["bg"],
            font=("monospace", 9, "bold"),
            relief="flat",
            width=5,
            cursor="hand2",
        ).pack(side="left", padx=3)

    def _entry_row(self, label, cmd, hint, row):
        fr = tk.Frame(self, bg=C["bg"])
        fr.grid(row=row, column=0, columnspan=3, sticky="ew", padx=15, pady=2)
        tk.Label(
            fr,
            text=label,
            bg=C["bg"],
            fg=C["fg"],
            font=("monospace", 10),
            width=12,
            anchor="w",
        ).pack(side="left")
        ent = tk.Entry(
            fr,
            bg=C["surface"],
            fg=C["fg"],
            font=("monospace", 10),
            insertbackground=C["fg"],
            width=8,
            relief="flat",
        )
        ent.pack(side="left", padx=3)
        tk.Label(fr, text=hint, bg=C["bg"], fg=C["dim"], font=("monospace", 8)).pack(
            side="left", padx=2
        )
        tk.Button(
            fr,
            text="OK",
            command=lambda: self._send_entry(cmd, ent),
            bg=C["blue"],
            fg=C["bg"],
            font=("monospace", 8, "bold"),
            relief="flat",
            width=4,
            cursor="hand2",
        ).pack(side="left", padx=5)

    def _send_entry(self, cmd, ent):
        val = ent.get().strip()
        if val:
            self._send(f"{cmd} {val}")

    def _btn_row(self, labels, row):
        fr = tk.Frame(self, bg=C["bg"])
        fr.grid(row=row, column=0, columnspan=3, sticky="w", padx=15, pady=2)
        cmds = {
            "Scan": "scan",
            "Identify": "identify",
            "Test": "test",
            "Göster": "hall",
            "Map": "map",
            "Save Map": "save",
            "Reload Map": "reload",
            "Reset Map": "mapreset",
            "Save Cfg": "savecfg",
            "Load Cfg": "loadcfg",
            "Defaults": "defaults",
            "Save All": "saveall",
        }
        for lbl in labels:
            cmd = cmds.get(lbl, lbl.lower())
            tk.Button(
                fr,
                text=lbl,
                command=lambda c=cmd: self._send(c),
                bg=C["overlay"],
                fg=C["fg"],
                font=("monospace", 9),
                relief="flat",
                padx=10,
                cursor="hand2",
            ).pack(side="left", padx=3)

    def _build_ui(self):
        r = 0
        self._section("Kick Ayarları", r)
        r += 1
        self._toggle_row("Kick", "kick on", "kick off", r)
        r += 1
        self._entry_row("Kick Duty", "kickduty", "0-255", r)
        r += 1
        self._entry_row("Kick Ms", "kickms", "0-3000", r)
        r += 1

        self._section("Ramp Ayarları", r)
        r += 1
        self._toggle_row("Ramp", "ramp on", "ramp off", r)
        r += 1
        self._entry_row("Ramp Rate", "ramprate", "1-64", r)
        r += 1
        self._entry_row("Ramp Ms", "rampms", "1-1000", r)
        r += 1

        self._section("Genel", r)
        r += 1
        self._entry_row("Default PWM", "defpwm", "0-255", r)
        r += 1

        self._section("Servis Görevleri", r)
        r += 1
        self._btn_row(["Scan", "Identify", "Test"], r)
        r += 1

        self._section("Hall İşlemleri", r)
        r += 1
        self._btn_row(["Göster", "Map", "Save Map"], r)
        r += 1
        self._btn_row(["Reload Map", "Reset Map"], r)
        r += 1

        self._section("Kayıt", r)
        r += 1
        self._btn_row(["Save Cfg", "Load Cfg", "Defaults"], r)
        r += 1
        self._btn_row(["Save All"], r)
        r += 1

        # Sürüşe Dön — FIX #2: mode control gönder, hareket otomatik başlamaz
        r += 1
        tk.Button(
            self,
            text=" ◀  Sürüşe Dön  (mode control)",
            command=self._on_close,
            bg=C["green"],
            fg=C["bg"],
            font=("monospace", 13, "bold"),
            relief="flat",
            padx=15,
            pady=10,
            cursor="hand2",
        ).grid(row=r, column=0, columnspan=3, pady=10)

        # Serial Monitör Log
        r += 1
        tk.Label(
            self,
            text="── Serial Monitör ──",
            bg=C["bg"],
            fg=C["cyan"],
            font=("monospace", 11, "bold"),
            anchor="w",
        ).grid(row=r, column=0, columnspan=3, sticky="w", padx=15, pady=(10, 2))

        r += 1
        self.log_text = scrolledtext.ScrolledText(
            self,
            bg=C["surface"],
            fg=C["fg"],
            font=("monospace", 9),
            height=8,
            width=50,
            state="normal",
            wrap="word",
            relief="flat",
        )
        self.log_text.grid(row=r, column=0, columnspan=3, padx=15, pady=5, sticky="ew")

    # FIX #2: Settings'ten güvenli dönüş
    def _on_close(self):
        # Log referansını temizle
        self.ctrl.settings_log = None
        # Control moduna dön — motor durmuş kalır, kullanıcı basmadan yürümez
        self.ctrl.send_command("mode control")
        time.sleep(0.1)
        self.destroy()


# ═══════════════════════════════════════════════════════════════════
#  MotorGUI — Ana pencere
# ═══════════════════════════════════════════════════════════════════
class MotorGUI(tk.Tk):
    """Ana sürüş penceresi. WASD+/− klavye, fare ile PWM slider,
    Settings penceresi açma, Identify, Brake (placeholder)."""

    def __init__(self, port, baud):
        super().__init__()

        self.title("BLDC Motor Kontrol")
        self.geometry("820x540")
        self.configure(bg=C["bg"])
        self.resizable(False, False)

        try:
            self.ctrl = MotorController(port, baud)
        except serial.SerialException as e:
            self.destroy()
            print(f"Serial hata: {e}")
            sys.exit(1)

        self._port = port
        self._baud = baud

        # Tuş takibi — hareket ve PWM bağımsız kanallar
        self.pressed_keys = set()

        # Settings penceresi referansı
        self.settings_win = None

        # FIX #5: PWM slider throttle state
        self._pwm_timer_id = None
        self._pending_pwm = 150
        self._last_pwm_sent = 150

        self._build_ui()
        self._bind_events()
        self._start_threads()
        self._tick()

    # ── UI Oluşturma ──────────────────────────────────────────────
    def _build_ui(self):
        # Üst bar
        top = tk.Frame(self, bg=C["surface"], height=28)
        top.pack(fill="x")
        top.pack_propagate(False)

        tk.Label(
            top,
            text=f"  {self._port} @ {self._baud}",
            bg=C["surface"],
            fg=C["dim"],
            font=("monospace", 9),
        ).pack(side="left", padx=10)

        self.mode_lbl = tk.Label(
            top,
            text=" BEKLE.. ",
            bg=C["red"],
            fg=C["bg"],
            font=("monospace", 9, "bold"),
            padx=8,
        )
        self.mode_lbl.pack(side="right", padx=10)

        # Ana içerik — sol + sağ
        body = tk.Frame(self, bg=C["bg"])
        body.pack(fill="both", expand=True, padx=15, pady=10)

        # ── Sol Panel ──
        left = tk.Frame(body, bg=C["bg"])
        left.pack(side="left", fill="both", expand=True)

        tk.Label(
            left,
            text="── Kontroller ──",
            bg=C["bg"],
            fg=C["dim"],
            font=("monospace", 10),
        ).pack(anchor="w", pady=(0, 5))

        kf = tk.Frame(left, bg=C["bg"])
        kf.pack(anchor="w")

        self.key_boxes = {}
        self.key_boxes["w"] = self._key_box(kf, "W", "İleri", 0, 0)
        self.key_boxes["a"] = self._key_box(kf, "A", "───", 1, 0)
        self.key_boxes["s"] = self._key_box(kf, "S", "Geri", 1, 1)
        self.key_boxes["d"] = self._key_box(kf, "D", "───", 1, 2)

        pm = tk.Frame(kf, bg=C["bg"])
        pm.grid(row=2, column=0, columnspan=3, pady=5, sticky="w")
        self._key_box(pm, "+", "PWM +10", 0, 0)
        self._key_box(pm, "-", "PWM -10", 0, 1)
        self._key_box(pm, "SPC", "Brake*", 0, 2)
        self._key_box(pm, "I", "Identify", 0, 3)

        # Motor durum
        tk.Label(left, text="", bg=C["bg"]).pack()
        tk.Label(
            left, text="── Motor ──", bg=C["bg"], fg=C["dim"], font=("monospace", 10)
        ).pack(anchor="w", pady=(5, 2))

        self.dir_lbl = tk.Label(
            left,
            text="─── DURDU ───",
            bg=C["bg"],
            fg=C["yellow"],
            font=("monospace", 12, "bold"),
        )
        self.dir_lbl.pack(anchor="w")

        self.phase_lbl = tk.Label(
            left, text="Faz: STOPPED", bg=C["bg"], fg=C["cyan"], font=("monospace", 10)
        )
        self.phase_lbl.pack(anchor="w")

        self.hall_lbl = tk.Label(
            left, text="Hall: -", bg=C["bg"], fg=C["dim"], font=("monospace", 9)
        )
        self.hall_lbl.pack(anchor="w")

        # RPM
        tk.Label(left, text="", bg=C["bg"]).pack()
        tk.Label(
            left, text="── RPM ──", bg=C["bg"], fg=C["dim"], font=("monospace", 10)
        ).pack(anchor="w", pady=(5, 2))

        rf = tk.Frame(left, bg=C["bg"])
        rf.pack(anchor="w", fill="x")

        self.rpm_lbl = tk.Label(
            rf,
            text="    0",
            bg=C["bg"],
            fg=C["green"],
            font=("monospace", 14, "bold"),
            width=6,
        )
        self.rpm_lbl.pack(side="left")

        self.rpm_bar = tk.Canvas(
            rf, bg=C["surface"], height=18, width=200, highlightthickness=0
        )
        self.rpm_bar.pack(side="left", padx=8, fill="x", expand=True)

        # ── Sağ Panel ──
        right = tk.Frame(body, bg=C["bg"], width=300)
        right.pack(side="right", fill="y")
        right.pack_propagate(False)

        # PWM Slider
        tk.Label(
            right,
            text="── PWM (Mouse) ──",
            bg=C["bg"],
            fg=C["dim"],
            font=("monospace", 10),
        ).pack(anchor="w", pady=(0, 5))

        self.pwm_scale = tk.Scale(
            right,
            from_=0,
            to=255,
            orient="horizontal",
            command=self._on_slider_change,
            bg=C["bg"],
            fg=C["fg"],
            troughcolor=C["surface"],
            activebackground=C["cyan"],
            highlightthickness=0,
            showvalue=False,
            length=280,
            sliderlength=25,
        )
        self.pwm_scale.set(150)
        self.pwm_scale.pack(fill="x")

        self.pwm_lbl = tk.Label(
            right,
            text="150 / 255  (58%)",
            bg=C["bg"],
            fg=C["cyan"],
            font=("monospace", 11, "bold"),
        )
        self.pwm_lbl.pack()

        # PWM aktif bar
        tk.Label(
            right, text="Aktif:", bg=C["bg"], fg=C["dim"], font=("monospace", 9)
        ).pack(anchor="w", pady=(8, 0))

        self.pwm_act_bar = tk.Canvas(
            right, bg=C["surface"], height=14, width=280, highlightthickness=0
        )
        self.pwm_act_bar.pack(anchor="w")

        self.pwm_act_lbl = tk.Label(
            right, text="0 / 255", bg=C["bg"], fg=C["yellow"], font=("monospace", 10)
        )
        self.pwm_act_lbl.pack(anchor="w")

        # ── Butonlar ──
        tk.Label(right, text="", bg=C["bg"]).pack()
        bf = tk.Frame(right, bg=C["bg"])
        bf.pack(fill="x")

        tk.Button(
            bf,
            text="Identify",
            command=lambda: self.ctrl.send_command("identify"),
            bg=C["magenta"],
            fg=C["bg"],
            font=("monospace", 10, "bold"),
            relief="flat",
            padx=15,
            cursor="hand2",
        ).pack(fill="x", pady=3)

        tk.Button(
            bf,
            text="⚙  Ayarlar",
            command=self._open_settings,
            bg=C["blue"],
            fg=C["bg"],
            font=("monospace", 10, "bold"),
            relief="flat",
            padx=15,
            cursor="hand2",
        ).pack(fill="x", pady=3)

        # FIX #9: Brake — placeholder, Phase 4, disabled
        self.brake_btn = tk.Button(
            bf,
            text="Brake  (Phase 4)",
            state="disabled",
            bg=C["overlay"],
            fg=C["dim"],
            font=("monospace", 10),
            relief="flat",
            padx=15,
        )
        self.brake_btn.pack(fill="x", pady=3)

        # Komut satırı
        cmd_fr = tk.Frame(self, bg=C["bg"])
        cmd_fr.pack(fill="x", side="bottom", pady=(0, 5))

        tk.Label(
            cmd_fr,
            text=">",
            bg=C["bg"],
            fg=C["cyan"],
            font=("monospace", 11, "bold"),
        ).pack(side="left", padx=(10, 2))

        self.cmd_entry = tk.Entry(
            cmd_fr,
            bg=C["surface"],
            fg=C["fg"],
            font=("monospace", 10),
            insertbackground=C["fg"],
            relief="flat",
        )
        self.cmd_entry.pack(side="left", fill="x", expand=True, padx=2)
        self.cmd_entry.bind("<Return>", lambda e: self._send_cmd_entry())

        tk.Button(
            cmd_fr,
            text="Gönder",
            command=self._send_cmd_entry,
            bg=C["blue"],
            fg=C["bg"],
            font=("monospace", 9, "bold"),
            relief="flat",
            padx=10,
            cursor="hand2",
        ).pack(side="left", padx=(2, 10))

        # Alt bar
        bot = tk.Frame(self, bg=C["surface"], height=22)
        bot.pack(fill="x", side="bottom")
        bot.pack_propagate(False)

        self.stat_lbl = tk.Label(
            bot,
            text=f"Port: {self._port}  |  Cmd: 0  Tel: 0",
            bg=C["surface"],
            fg=C["dim"],
            font=("monospace", 8),
        )
        self.stat_lbl.pack(side="left", padx=10)

        tk.Label(
            bot,
            text="Esc/Q: Çıkış",
            bg=C["surface"],
            fg=C["dim"],
            font=("monospace", 8),
        ).pack(side="right", padx=10)

    def _key_box(self, parent, key, desc, row, col):
        fr = tk.Frame(parent, bg=C["surface"], relief="flat")
        fr.grid(row=row, column=col, padx=3, pady=3)
        tk.Label(
            fr,
            text=key,
            bg=C["surface"],
            fg=C["fg"],
            font=("monospace", 12, "bold"),
            width=4,
        ).pack(pady=(4, 0))
        tk.Label(
            fr, text=desc, bg=C["surface"], fg=C["dim"], font=("monospace", 7)
        ).pack(pady=(0, 4))
        return fr

    # ── FIX #3: Güvenilir event binding ───────────────────────────
    def _bind_events(self):
        # bind_all ile tüm widget'ları kapsar, ama handler filtreler
        self.bind_all("<KeyPress>", self._on_press)
        self.bind_all("<KeyRelease>", self._on_release)

        # Focus kaybı / minimize → güvenli stop
        self.bind("<FocusOut>", self._on_focus_out)
        self.bind("<Unmap>", self._on_unmap)
        self.bind("<Escape>", lambda e: self._quit())

    def _event_from_main_window(self, ev):
        """Event ana pencereden geliyorsa True."""
        try:
            return ev.widget.winfo_toplevel() == self
        except (tk.TclError, AttributeError):
            return False

    # ── FIX #3 + #4: Key press — güvenilir yakalama ──────────────
    def _on_press(self, ev):
        # Ana pencere focus yoksa yakalama (settings Entry vs.)
        if not self._event_from_main_window(ev):
            return

        # Entry/Text widget'ında yazarken motor komutlarını engelle
        if isinstance(ev.widget, (tk.Entry, tk.Text)):
            return

        k = ev.keysym.lower()

        # FIX: Settings modundayken sadece Escape/Q'ya izin ver
        # Firmware settings modunda da komut işleyebilir, ancak GUI güvenlik için
        # hareket girişini engeller. Ayarlar penceresindeyken motor kontrolü
        # istenmeyen durumlara yol açabilir.
        if self.ctrl.current_mode == "SETTINGS":
            if k in ("escape", "q"):
                self._quit()
            return

        # Auto-repeat koruması
        if k in self.pressed_keys:
            return
        self.pressed_keys.add(k)

        # ── Hareket kanalı (PWM kanalından bağımsız) ──
        if k == "w":
            self.ctrl.forward()
            self._set_key_active("w", True)
        elif k == "s":
            self.ctrl.backward()
            self._set_key_active("s", True)

        # ── PWM kanalı (hareket kanalından bağımsız) ──
        # FIX: +/- basıldığında _last_pwm_sent güncellenir,
        # böylece slider throttle timer duplicate komut göndermez
        elif k in ("plus", "equal", "kp_add"):
            self.ctrl.pwm_up()
            self._last_pwm_sent = self.ctrl.pwm_set
            self._pending_pwm = self.ctrl.pwm_set
            self.pwm_scale.set(self.ctrl.pwm_set)
        elif k in ("minus", "kp_subtract", "underscore"):
            self.ctrl.pwm_down()
            self._last_pwm_sent = self.ctrl.pwm_set
            self._pending_pwm = self.ctrl.pwm_set
            self.pwm_scale.set(self.ctrl.pwm_set)

        # ── Servis komutları ──
        elif k == "i":
            self.ctrl.send_command("identify")
        elif k == "space":
            # FIX #9: Brake placeholder — şimdilik hiçbir şey yapmaz
            pass
        elif k == "q":
            self._quit()

    # ── FIX #4: Key release — temiz W/S switch mantığı ──────────
    def _on_release(self, ev):
        if not self._event_from_main_window(ev):
            return

        k = ev.keysym.lower()
        self.pressed_keys.discard(k)

        if k == "w":
            self._set_key_active("w", False)
            if "s" in self.pressed_keys:
                # S hala basılı → geriye devam
                self.ctrl.backward()
            else:
                # Hiç hareket tuşu kalmadı → stop
                self.ctrl.stop()

        elif k == "s":
            self._set_key_active("s", False)
            if "w" in self.pressed_keys:
                # W hala basılı → ileriye devam
                self.ctrl.forward()
            else:
                # Hiç hareket tuşu kalmadı → stop
                self.ctrl.stop()

    def _set_key_active(self, key, active):
        box = self.key_boxes.get(key)
        if not box:
            return
        if active:
            box.configure(bg=C["green"])
            for ch in box.winfo_children():
                ch.configure(bg=C["green"], fg=C["bg"])
        else:
            box.configure(bg=C["surface"])
            for ch in box.winfo_children():
                ch.configure(bg=C["surface"], fg=C["fg"])

    # ── FIX #3: Focus kaybı → güvenli stop ───────────────────────
    def _on_focus_out(self, _ev):
        # Settings penceresine tıklanınca focus kaybı normal — motor zaten durmuş
        if self.ctrl.movement is not None:
            self._safe_stop()

    def _on_unmap(self, _ev):
        """Pencere minimize edilince motoru durdur."""
        if self.ctrl.movement is not None:
            self._safe_stop()

    # ── FIX #1: Güvenli durdurma helper ──────────────────────────
    def _safe_stop(self):
        """Motoru durdur, hareket state temizle, tuş görselleri sıfırla.
        Settings açılırken, focus kaybında, minimize'da, kapanışta kullanılır.
        Movement=None önce set edilir ki heartbeat f/b göndermesin."""
        self.ctrl.movement = None
        self.ctrl.direction = 0
        self.ctrl.stop()  # "s" gönderir
        self.pressed_keys.clear()
        self._set_key_active("w", False)
        self._set_key_active("s", False)

    def _send_cmd_entry(self):
        """Komut satırından seri porta komut gönder."""
        cmd = self.cmd_entry.get().strip()
        if cmd:
            self.ctrl.send_command(cmd)
            self.cmd_entry.delete(0, "end")

    # ── FIX #5: PWM slider throttle ──────────────────────────────
    def _on_slider_change(self, val):
        """Slider her hareket ettiğinde tetiklenir.
        UI hemen güncellenir (akıcı), seri komut throttle edilir.
        +/- tuşundan sonra _last_pwm_sent güncellendiği için
        bu callback duplicate komut göndermez."""
        try:
            v = int(val)
        except (ValueError, TypeError):
            return

        # UI'yi hemen güncelle
        pct = int(v / 255 * 100)
        self.pwm_lbl.configure(text=f"{v} / 255  ({pct}%)")

        # Değer zaten gönderildiyse timer oluşturma (telemetri sync / +/- tuşu)
        if v == self._last_pwm_sent:
            return

        # Seri komutu throttle et — son değer gönderilmeden yeni gelirse timer resetle
        if self._pwm_timer_id:
            self.after_cancel(self._pwm_timer_id)
        self._pending_pwm = v
        self._pwm_timer_id = self.after(PWM_THROTTLE_MS, self._flush_pwm)

    def _flush_pwm(self):
        """Throttle sonunda bekleyen PWM değerini seri porta gönder."""
        self._pwm_timer_id = None
        if self._pending_pwm != self._last_pwm_sent:
            self.ctrl.pwm_set = self._pending_pwm
            self.ctrl.send_command(f"pwm {self._pending_pwm}")
            self._last_pwm_sent = self._pending_pwm

    # ── FIX #1 + #2: Settings aç — motor durdur → ayarlar ───────
    def _open_settings(self):
        if self.settings_win and self.settings_win.winfo_exists():
            self.settings_win.lift()
            return
        # Motoru güvenli durdur — heartbeat kesilir
        self._safe_stop()
        # Settings penceresi kendi __init__'inde "mode settings" gönderir
        self.settings_win = SettingsWindow(self, self.ctrl)

    # ── Thread'ler ────────────────────────────────────────────────
    def _start_threads(self):
        self.ctrl.activate_control_mode()

        t1 = threading.Thread(target=self.ctrl.read_telemetry_loop, daemon=True)
        t1.start()

        t2 = threading.Thread(target=self.ctrl._heartbeat_loop, daemon=True)
        t2.start()

    # ── FIX #8: UI Tick — Her 80ms'de telemetri senkronize ───────
    def _tick(self):
        c = self.ctrl

        # Mod etiketi
        if c.mode_confirmed:
            self.mode_lbl.configure(text=" CONTROL ", bg=C["green"], fg=C["bg"])
        elif c.current_mode == "SETTINGS":
            self.mode_lbl.configure(text=" SETTINGS ", bg=C["yellow"], fg=C["bg"])
        else:
            self.mode_lbl.configure(text=" BEKLE.. ", bg=C["red"], fg=C["fg"])

        # Yön
        if c.direction == 1:
            self.dir_lbl.configure(text="▶▶▶ İLERİ ▶▶▶", fg=C["green"])
        elif c.direction == -1:
            self.dir_lbl.configure(text="◀◀◀ GERİ  ◀◀◀", fg=C["red"])
        else:
            self.dir_lbl.configure(text="─── DURDU ───", fg=C["yellow"])

        # Faz
        phase_colors = {
            "STOPPED": C["cyan"],
            "KICK": C["yellow"],
            "RUNNING": C["green"],
            "NEUTRAL_WAIT": C["yellow"],
            "FAULT": C["red"],
            "BRAKE": C["magenta"],
        }
        self.phase_lbl.configure(
            text=f"Faz: {c.phase_name}", fg=phase_colors.get(c.phase_name, C["fg"])
        )

        # Hall
        self.hall_lbl.configure(text=f"Hall: {c.hall_raw}")

        # RPM
        self.rpm_lbl.configure(text=f"{c.rpm:5d}")
        self.rpm_bar.delete("bar")
        w = self.rpm_bar.winfo_width()
        if w > 2:
            pct = min(1.0, c.rpm / RPM_MAX_DISPLAY)
            self.rpm_bar.create_rectangle(
                0, 0, int(pct * w), 18, fill=C["green"], tags="bar", outline=""
            )

        # PWM label — slider ile senkron
        pct = int(c.pwm_set / 255 * 100)
        self.pwm_lbl.configure(text=f"{c.pwm_set} / 255  ({pct}%)")

        # Slider senkron (throttle callback'i tetiklemeden)
        # FIX: Pending timer iptal et, _last_pwm_sent güncelle,
        # böylece _on_slider_change gereksiz timer başlatmaz
        current_slider = self.pwm_scale.get()
        if abs(current_slider - c.pwm_set) > 1:
            if self._pwm_timer_id:
                self.after_cancel(self._pwm_timer_id)
                self._pwm_timer_id = None
            self._pending_pwm = c.pwm_set
            self._last_pwm_sent = c.pwm_set
            self.pwm_scale.set(c.pwm_set)

        # PWM aktif bar
        self.pwm_act_bar.delete("bar")
        bw = self.pwm_act_bar.winfo_width()
        if bw > 2:
            ap = c.pwm_act / 255.0
            self.pwm_act_bar.create_rectangle(
                0, 0, int(ap * bw), 14, fill=C["yellow"], tags="bar", outline=""
            )
        self.pwm_act_lbl.configure(text=f"{c.pwm_act} / 255")

        # Settings log queue'sunu boşalt (thread-safe)
        log_lines = []
        with c._log_lock:
            log_lines = c._log_queue[:]
            c._log_queue.clear()
        if c.settings_log is not None and log_lines:
            for line in log_lines:
                try:
                    c.settings_log.insert("end", line + "\n")
                except Exception:
                    pass
            try:
                c.settings_log.see("end")
            except Exception:
                pass

        # Alt bar
        if c.error_msg:
            self.stat_lbl.configure(text=f"⚠ {c.error_msg}")
            c.error_msg = ""
        else:
            self.stat_lbl.configure(
                text=f"Port: {self._port}  |  Cmd: {c.cmd_count}  Tel: {c.telem_count}"
            )

        self.after(80, self._tick)

    # ── FIX #7: Temiz kapanış ────────────────────────────────────
    def _quit(self):
        # Settings açıksa kapat
        if self.settings_win and self.settings_win.winfo_exists():
            try:
                self.ctrl.send_command("mode control")
                time.sleep(0.05)
                self.settings_win.destroy()
            except Exception:
                pass
        # Motoru durdur, normal moda dön
        self._safe_stop()
        time.sleep(0.05)
        self.ctrl.close()
        self.destroy()


# ═══════════════════════════════════════════════════════════════════
#  Main
# ═══════════════════════════════════════════════════════════════════
if __name__ == "__main__":
    port = DEFAULT_PORT
    baud = DEFAULT_BAUD

    if len(sys.argv) > 1:
        port = sys.argv[1]
    if len(sys.argv) > 2:
        try:
            baud = int(sys.argv[2])
        except ValueError:
            pass

    gui = MotorGUI(port, baud)
    gui.mainloop()
