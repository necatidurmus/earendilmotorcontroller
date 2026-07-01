#!/usr/bin/env python3
"""F446 -> F411 single motor GUI.

Usage:
    python tools/f446_motor_gui.py
    python tools/f446_motor_gui.py --port /dev/ttyACM0

Requires: pip install pyserial
"""

import argparse
import math
import queue
import re
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


def serial_ports():
    if list_ports is None:
        return []
    return sorted(p.device for p in list_ports.comports())


def safe_int(val, default=0):
    try:
        return int(val)
    except (ValueError, TypeError):
        return default


# RPM gauge maximum (configurable)
GUI_RPM_GAUGE_MAX = 500

# Minimum interval between brake commands (ms) — prevents spam
BRAKE_DEBOUNCE_MS = 200


class SerialClient:
    def __init__(self):
        self.ser = None
        self.rx_queue = queue.Queue(maxsize=2000)
        self.lock = threading.Lock()
        self.running = False
        self.reader = None

    def connect(self, port, baud):
        if serial is None:
            raise RuntimeError("pyserial missing. Run: pip install pyserial")
        self.disconnect()
        self.ser = serial.Serial(port=port, baudrate=baud, timeout=0.1)
        self.running = True
        self.reader = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader.start()

    def disconnect(self):
        self.running = False
        if self.reader and self.reader.is_alive():
            self.reader.join(timeout=0.5)
        self.reader = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def is_connected(self):
        return self.ser is not None and self.ser.is_open

    def send(self, line):
        line = line.strip()
        if not line:
            return
        with self.lock:
            if not self.is_connected():
                raise RuntimeError("Serial not connected")
            self.ser.write((line + "\n").encode("utf-8"))
            self.ser.flush()

    def _reader_loop(self):
        buf = b""
        while self.running and self.ser and self.ser.is_open:
            try:
                chunk = self.ser.read(256)
                if not chunk:
                    continue
                buf += chunk
                if len(buf) > 4096:
                    buf = buf[-4096:]
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.rstrip(b"\r").decode("utf-8", errors="replace")
                    try:
                        self.rx_queue.put_nowait(line)
                    except queue.Full:
                        try:
                            self.rx_queue.get_nowait()
                        except queue.Empty:
                            pass
                        try:
                            self.rx_queue.put_nowait(line)
                        except queue.Full:
                            pass
            except Exception as exc:
                try:
                    self.rx_queue.put_nowait(f"ERR|reader stopped: {exc}")
                except queue.Full:
                    pass
                break


class App(tk.Tk):
    def __init__(self, default_port=""):
        super().__init__()
        self.title("F446 -> F411 Motor GUI")
        self.geometry("920x640")

        self.client = SerialClient()
        self.heartbeat_cmd = None
        self.heartbeat_interval_ms = 300
        self.last_heartbeat_ms = 0

        self.telemetry_vars = {
            "RPM":     tk.StringVar(value="--"),
            "T":       tk.StringVar(value="--"),
            "D":       tk.StringVar(value="--"),
            "DIR":     tk.StringVar(value="--"),
            "APP_PH":  tk.StringVar(value="--"),
            "SP":      tk.StringVar(value="--"),
            "BRAKE":   tk.StringVar(value="--"),
            "FC":      tk.StringVar(value="--"),
            "H":       tk.StringVar(value="--"),
            "PWM_SET": tk.StringVar(value="--"),
            "PWM_ACT": tk.StringVar(value="--"),
        }

        # PI tuning vars
        self.pi_kp = tk.StringVar(value="0.800")
        self.pi_ki = tk.StringVar(value="0.050")
        self.pi_base = [tk.StringVar(value=v) for v in
                        ("640", "660", "680", "700", "720", "700", "670", "640")]
        self.pi_boost = [tk.StringVar(value=v) for v in
                         ("880", "900", "920", "940", "960", "990", "1020", "1040")]
        self.pi_boost_ms = tk.StringVar(value="150")
        self.pi_ramp_up = tk.StringVar(value="60")
        self.pi_ramp_down = tk.StringVar(value="150")

        self._default_port = default_port
        self._last_telem_ms = 0
        self._telem_stale = True
        self._last_brake_ms = 0          # brake debounce
        self._build_ui()
        self._bind_keys()
        self.after(50, self._tick)

    def _build_ui(self):
        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")

        ttk.Label(top, text="Port").pack(side="left")
        ports = serial_ports()
        default = self._default_port if self._default_port else (ports[0] if ports else "")
        self.port_var = tk.StringVar(value=default)
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, values=ports, width=18)
        self.port_combo.pack(side="left", padx=4)

        ttk.Button(top, text="Refresh", command=self._refresh_ports).pack(side="left", padx=2)

        ttk.Label(top, text="Baud").pack(side="left", padx=(12, 2))
        self.baud_var = tk.StringVar(value="115200")
        ttk.Entry(top, textvariable=self.baud_var, width=8).pack(side="left")

        self.connect_btn = ttk.Button(top, text="Connect", command=self._connect)
        self.connect_btn.pack(side="left", padx=8)
        self.disconnect_btn = ttk.Button(top, text="Disconnect", command=self._disconnect, state="disabled")
        self.disconnect_btn.pack(side="left")

        ttk.Button(top, text="PING", command=lambda: self._send("ping")).pack(side="left", padx=(20, 2))
        ttk.Button(top, text="HELP", command=lambda: self._send("help")).pack(side="left", padx=2)

        main = ttk.Frame(self, padding=8)
        main.pack(fill="both", expand=True)

        left = ttk.LabelFrame(main, text="Motor Control", padding=10)
        left.pack(side="left", fill="y", padx=(0, 8))

        self.mode_var = tk.StringVar(value="duty")
        ttk.Radiobutton(left, text="Duty mode", variable=self.mode_var, value="duty").grid(row=0, column=0, sticky="w")
        ttk.Radiobutton(left, text="Speed/RPM mode", variable=self.mode_var, value="rpm").grid(row=0, column=1, sticky="w")

        ttk.Label(left, text="PWM (0-4000)").grid(row=1, column=0, sticky="w", pady=(12, 0))
        self.pwm_var = tk.IntVar(value=320)
        ttk.Scale(left, from_=0, to=4000, variable=self.pwm_var, orient="horizontal", length=220).grid(row=2, column=0, columnspan=2)
        ttk.Entry(left, textvariable=self.pwm_var, width=8).grid(row=2, column=2, padx=4)

        ttk.Label(left, text="RPM (0-500)").grid(row=3, column=0, sticky="w", pady=(12, 0))
        self.rpm_var = tk.IntVar(value=30)
        ttk.Scale(left, from_=0, to=500, variable=self.rpm_var, orient="horizontal", length=220).grid(row=4, column=0, columnspan=2)
        ttk.Entry(left, textvariable=self.rpm_var, width=8).grid(row=4, column=2, padx=4)

        ttk.Label(left, text="Heartbeat ms").grid(row=5, column=0, sticky="w", pady=(12, 0))
        self.hb_var = tk.IntVar(value=300)
        ttk.Entry(left, textvariable=self.hb_var, width=8).grid(row=5, column=1, sticky="w")

        btns = ttk.Frame(left)
        btns.grid(row=6, column=0, columnspan=3, pady=16)

        ttk.Button(btns, text="Forward", command=self._forward).grid(row=0, column=0, padx=4, pady=4)
        ttk.Button(btns, text="Backward", command=self._backward).grid(row=0, column=1, padx=4, pady=4)
        ttk.Button(btns, text="STOP", command=self._stop).grid(row=1, column=0, padx=4, pady=4)
        ttk.Button(btns, text="E-STOP", command=self._estop).grid(row=1, column=1, padx=4, pady=4)

        ttk.Button(left, text="mode duty", command=lambda: self._send("m1 mode duty")).grid(row=7, column=0, sticky="ew", pady=2)
        ttk.Button(left, text="mode speed", command=lambda: self._send("m1 mode speed")).grid(row=7, column=1, sticky="ew", pady=2)
        ttk.Button(left, text="kick off", command=lambda: self._send("m1 kick off")).grid(row=8, column=0, sticky="ew", pady=2)
        ttk.Button(left, text="hall", command=lambda: self._send("m1 hall")).grid(row=8, column=1, sticky="ew", pady=2)
        ttk.Button(left, text="status", command=lambda: self._send("m1 status")).grid(row=9, column=0, sticky="ew", pady=2)
        ttk.Button(left, text="clrerr", command=lambda: self._send("m1 clrerr")).grid(row=9, column=1, sticky="ew", pady=2)

        # Brake / Identify buttons (F446 -> F411)
        action = ttk.Frame(left)
        action.grid(row=10, column=0, columnspan=3, pady=6, sticky="ew")

        self.brake_btn = ttk.Button(action, text="BRAKE [B]",
                                     command=self._do_brake)
        self.brake_btn.pack(side="left", padx=4)

        self.identify_btn = ttk.Button(action, text="IDENTIFY [I]",
                                        command=self._do_identify)
        self.identify_btn.pack(side="left", padx=4)

        self.unlock_btn = ttk.Button(action, text="Unlock Service",
                                      command=self._do_unlock_service)
        self.unlock_btn.pack(side="left", padx=4)

        raw = ttk.LabelFrame(left, text="Raw command", padding=8)
        raw.grid(row=11, column=0, columnspan=3, sticky="ew", pady=(14, 0))
        self.raw_var = tk.StringVar(value="status")
        ttk.Entry(raw, textvariable=self.raw_var, width=28).pack(side="left", padx=(0, 4))
        ttk.Button(raw, text="to F411", command=self._send_raw_to_f411).pack(side="left", padx=2)
        ttk.Button(raw, text="to F446", command=self._send_raw_to_f446).pack(side="left", padx=2)

        right = ttk.Frame(main)
        right.pack(side="left", fill="both", expand=True)

        telem = ttk.LabelFrame(right, text="M1 Telemetry", padding=8)
        telem.pack(fill="x")
        keys = list(self.telemetry_vars.keys())
        for i, key in enumerate(keys):
            row = (i // 5) * 2
            col = i % 5
            ttk.Label(telem, text=key).grid(row=row, column=col, padx=8, sticky="w")
            ttk.Label(telem, textvariable=self.telemetry_vars[key],
                       font=("TkDefaultFont", 11, "bold")).grid(row=row + 1, column=col, padx=8, sticky="w")

        # RPM Gauge
        gauge_frame = ttk.LabelFrame(right, text="RPM Gauge", padding=8)
        gauge_frame.pack(fill="x", pady=(8, 0))
        self._build_rpm_gauge(gauge_frame)

        # PI Tuning panel
        pi_frame = ttk.LabelFrame(right, text="PI Tuning", padding=8)
        pi_frame.pack(fill="x", pady=(8, 0))
        self._build_pi_panel(pi_frame)

        console_frame = ttk.LabelFrame(right, text="Console", padding=8)
        console_frame.pack(fill="both", expand=True, pady=(8, 0))
        self.console = tk.Text(console_frame, height=18, wrap="none")
        self.console.pack(fill="both", expand=True)

    def _refresh_ports(self):
        ports = serial_ports()
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    # ── RPM Gauge ─────────────────────────────────────────────────────

    def _build_rpm_gauge(self, parent):
        """Canvas-based semicircular RPM gauge with needle."""
        self._gauge_size = 180
        self._gauge_canvas = tk.Canvas(
            parent, width=self._gauge_size, height=self._gauge_size // 2 + 30,
            bg="#1e1e2e", highlightthickness=0,
        )
        self._gauge_canvas.pack(pady=4)
        self._gauge_stale_lbl = ttk.Label(parent, text="", foreground="gray")
        self._gauge_stale_lbl.pack()
        self._draw_gauge(0, stale=True)

    def _draw_gauge(self, rpm, stale=False):
        """Draw the semicircular gauge with needle at the given RPM."""
        c = self._gauge_canvas
        c.delete("all")
        w = self._gauge_size
        h = self._gauge_size // 2 + 30
        cx = w // 2
        cy = h - 10
        r = w // 2 - 15

        # Arc background
        c.create_arc(cx - r, cy - r, cx + r, cy + r,
                     start=0, extent=180, style="arc",
                     outline="#45475a", width=4)

        # Tick marks and labels
        max_rpm = GUI_RPM_GAUGE_MAX
        for i in range(11):
            angle_deg = 180 - i * 18  # 0..180 degrees, left to right
            angle_rad = math.radians(angle_deg)
            x1 = cx + (r - 8) * math.cos(angle_rad)
            y1 = cy - (r - 8) * math.sin(angle_rad)
            x2 = cx + (r + 2) * math.cos(angle_rad)
            y2 = cy - (r + 2) * math.sin(angle_rad)
            c.create_line(x1, y1, x2, y2, fill="#585b70", width=2)
            if i % 2 == 0:
                val = int(max_rpm * i / 10)
                lx = cx + (r - 22) * math.cos(angle_rad)
                ly = cy - (r - 22) * math.sin(angle_rad)
                c.create_text(lx, ly, text=str(val), fill="#cdd6f4",
                              font=("monospace", 7))

        # Needle
        if stale:
            needle_color = "#585b70"
        else:
            needle_color = "#f38ba8" if rpm >= max_rpm else "#a6e3a1"

        rpm_clamped = max(0, min(rpm, max_rpm))
        needle_angle = 180 - (rpm_clamped / max_rpm) * 180
        needle_rad = math.radians(needle_angle)
        nx = cx + (r - 5) * math.cos(needle_rad)
        ny = cy - (r - 5) * math.sin(needle_rad)
        c.create_line(cx, cy, nx, ny, fill=needle_color, width=3,
                      capstyle="round")

        # Center dot
        c.create_oval(cx - 4, cy - 4, cx + 4, cy + 4, fill="#cdd6f4")

        # RPM value text
        c.create_text(cx, cy - 15, text=f"{rpm:.0f}",
                      fill="#f9e2af", font=("monospace", 14, "bold"))
        c.create_text(cx, cy - 0, text="RPM",
                      fill="#585b70", font=("monospace", 8))

        # Stale indicator
        if stale:
            self._gauge_stale_lbl.configure(text="STALE (no telemetry)")
        else:
            self._gauge_stale_lbl.configure(text="")

    # ── Brake / Identify / Unlock ─────────────────────────────────────

    def _do_brake(self):
        """Send brake command with debouncing to prevent spam."""
        now_ms = int(time.time() * 1000)
        if now_ms - self._last_brake_ms < BRAKE_DEBOUNCE_MS:
            return
        self._last_brake_ms = now_ms
        self._stop_heartbeat()
        self._send("m1 x")

    def _do_identify(self):
        """Send identify command. Auto-unlocks bridge service first."""
        if not messagebox.askyesno(
            "Identify",
            "This will energize motor phases to identify Hall mapping.\n\n"
            "Requirements:\n"
            "  - Current-limited bench supply\n"
            "  - Motor wheels unloaded\n"
            "  - Emergency stop ready\n\n"
            "The bridge service will be unlocked automatically.\n"
            "F411-side arming may be required separately.\n\n"
            "Continue?"
        ):
            return
        # Auto-unlock bridge service, arm F411, then send identify
        self._send("bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY")
        self.after(200, lambda: self._send("m1 arm service CURRENT_LIMITED_BENCH_SUPPLY"))
        self.after(500, lambda: self._send("m1 identify"))

    def _do_unlock_service(self):
        """Unlock service on F446 bridge for dangerous commands."""
        self._send("bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY")
        self._log("TX bridge unlock_service (30s window)")

    # ── PI Tuning Panel ────────────────────────────────────────────────

    def _build_pi_panel(self, parent):
        """Build PI tuning controls: gains, base, boost, ramp."""
        row = 0

        # Gains: Kp, Ki
        ttk.Label(parent, text="Kp").grid(row=row, column=0, sticky="w")
        ttk.Entry(parent, textvariable=self.pi_kp, width=8).grid(row=row, column=1, padx=2)
        ttk.Label(parent, text="Ki").grid(row=row, column=2, sticky="w", padx=(8, 0))
        ttk.Entry(parent, textvariable=self.pi_ki, width=8).grid(row=row, column=3, padx=2)
        row += 1

        # Base PWM: 8 RPM bands
        ttk.Label(parent, text="Base").grid(row=row, column=0, sticky="w", pady=(4, 0))
        for i, var in enumerate(self.pi_base):
            ttk.Entry(parent, textvariable=var, width=5).grid(
                row=row, column=i + 1, padx=2, pady=(4, 0))
        row += 1

        # Boost PWM: 8 RPM bands and one shared duration
        ttk.Label(parent, text="Boost").grid(row=row, column=0, sticky="w", pady=(4, 0))
        for i, var in enumerate(self.pi_boost):
            ttk.Entry(parent, textvariable=var, width=5).grid(
                row=row, column=i + 1, padx=2, pady=(4, 0))
        ttk.Label(parent, text="ms").grid(row=row, column=9, padx=(6, 0), pady=(4, 0))
        ttk.Entry(parent, textvariable=self.pi_boost_ms, width=5).grid(
            row=row, column=10, padx=2, pady=(4, 0))
        row += 1

        # Ramp: up, down
        ttk.Label(parent, text="Ramp").grid(row=row, column=0, sticky="w", pady=(4, 0))
        ttk.Entry(parent, textvariable=self.pi_ramp_up, width=8).grid(row=row, column=1, padx=2, pady=(4, 0))
        ttk.Label(parent, text="↓").grid(row=row, column=2, pady=(4, 0))
        ttk.Entry(parent, textvariable=self.pi_ramp_down, width=8).grid(row=row, column=3, padx=2, pady=(4, 0))
        row += 1

        # Buttons
        btn_row = ttk.Frame(parent)
        btn_row.grid(row=row, column=0, columnspan=11, pady=(8, 0), sticky="ew")
        self._apply_btn = ttk.Button(btn_row, text="Apply All", command=self._apply_pi)
        self._apply_btn.pack(side="left", padx=4)
        ttk.Button(btn_row, text="Read", command=self._read_pi).pack(side="left", padx=4)
        ttk.Button(btn_row, text="Save Config", command=self._save_config).pack(side="left", padx=4)
        ttk.Button(btn_row, text="Load Config", command=self._load_config).pack(side="left", padx=4)
        ttk.Label(btn_row, text="(Apply→RAM only, Save→flash)",
                  foreground="gray").pack(side="left", padx=(8, 0))

    def _apply_pi(self):
        """Send PI tuning parameters to F411 (auto-unlocks F446 service)."""
        try:
            kp = float(self.pi_kp.get())
            ki = float(self.pi_ki.get())
            base = [int(var.get()) for var in self.pi_base]
            boost = [int(var.get()) for var in self.pi_boost]
            boost_ms = int(self.pi_boost_ms.get())
            ramp_up = float(self.pi_ramp_up.get())
            ramp_down = float(self.pi_ramp_down.get())
        except ValueError:
            self._log("ERR|Invalid PI tuning value")
            return

        # F446 bridge blocks pi/base/boost as dangerous service commands.
        # Unlock first, then send commands with small delays so the unlock
        # is processed before the m1-prefixed commands arrive.
        self._send("bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY")
        self.after(150, lambda: self._send(f"m1 pi {kp:.3f} {ki:.3f}"))
        self.after(220, lambda: self._send("m1 base " + " ".join(map(str, base))))
        self.after(290, lambda: self._send(
            "m1 boost " + " ".join(map(str, boost)) + f" {boost_ms}"))
        self.after(360, lambda: self._send(f"m1 ramp {ramp_up:.0f} {ramp_down:.0f}"))
        self._log("Applied PI tuning to RAM. Use Save Config to persist after reset.")

    def _read_pi(self):
        """Request current PI tuning params from F411 via spstat."""
        self._send("m1 spstat")

    def _save_config(self):
        """Send savecfg to F411 to persist current runtime config to flash.
        Requires bridge service unlock if service lock is enabled."""
        if not messagebox.askyesno(
            "Save Config",
            "Save current runtime settings to F411 flash?\n\n"
            "This sends: m1 savecfg\n\n"
            "Requires bridge service unlock if service lock is enabled.\n"
            "Motor must be stopped.\n\n"
            "Continue?"
        ):
            return
        self._send("bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY")
        self.after(200, lambda: self._send("m1 savecfg"))
        self._log("Save Config command sent: m1 savecfg")

    def _load_config(self):
        """Send loadcfg to F411 to load saved config from flash into RAM.
        Requires bridge service unlock if service lock is enabled."""
        if not messagebox.askyesno(
            "Load Config",
            "Load saved config from F411 flash into RAM?\n\n"
            "This sends: m1 loadcfg\n\n"
            "Requires bridge service unlock if service lock is enabled.\n"
            "Motor must be stopped.\n\n"
            "Continue?"
        ):
            return
        self._send("bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY")
        self.after(200, lambda: self._send("m1 loadcfg"))
        self._log("Load Config command sent: m1 loadcfg")

    # ── Keyboard shortcuts ────────────────────────────────────────────

    def _bind_keys(self):
        def _guard(func):
            """Only trigger shortcut when no text entry has focus."""
            def wrapper(e):
                w = self.focus_get()
                if w and isinstance(w, (tk.Entry, ttk.Entry, tk.Text)):
                    return
                func()
            return wrapper

        self.bind("<KeyPress-b>", _guard(self._do_brake))
        self.bind("<KeyPress-B>", _guard(self._do_brake))
        self.bind("<KeyPress-i>", _guard(self._do_identify))
        self.bind("<KeyPress-I>", _guard(self._do_identify))

    def _connect(self):
        try:
            baud = safe_int(self.baud_var.get(), 115200)
            self.client.connect(self.port_var.get(), baud)
            self.connect_btn.configure(state="disabled")
            self.disconnect_btn.configure(state="normal")
            self._log("connected to " + self.port_var.get())
        except Exception as exc:
            messagebox.showerror("Connect error", str(exc))

    def _disconnect(self):
        self._stop_heartbeat()
        if self.client.is_connected():
            try:
                self.client.send("stop")
            except Exception:
                pass
        self.client.disconnect()
        self.connect_btn.configure(state="normal")
        self.disconnect_btn.configure(state="disabled")
        self._log("disconnected")

    def _send(self, line):
        try:
            self.client.send(line)
            self._log("TX " + line)
        except Exception as exc:
            self._log("SEND ERR " + str(exc))

    def _command_for(self, direction):
        if self.mode_var.get() == "rpm":
            rpm = max(0, min(500, safe_int(self.rpm_var.get(), 30)))
            signed = rpm if direction == "forward" else -rpm
            return f"m1 rpm {signed}"
        pwm = max(0, min(4000, safe_int(self.pwm_var.get(), 320)))
        letter = "f" if direction == "forward" else "b"
        return f"m1 {letter}{pwm}"

    def _forward(self):
        mode_name = "speed" if self.mode_var.get() == "rpm" else "duty"
        self._send(f"m1 mode {mode_name}")
        cmd = self._command_for("forward")
        self._send(cmd)
        self._start_heartbeat(cmd)

    def _backward(self):
        mode_name = "speed" if self.mode_var.get() == "rpm" else "duty"
        self._send(f"m1 mode {mode_name}")
        cmd = self._command_for("backward")
        self._send(cmd)
        self._start_heartbeat(cmd)

    def _stop(self):
        self._stop_heartbeat()
        self._send("stop")

    def _estop(self):
        """Send fault-latched emergency stop (clrerr required to recover)."""
        self._stop_heartbeat()
        self._send("estop")

    def _send_raw_to_f411(self):
        raw = self.raw_var.get().strip()
        if raw:
            # Dangerous command warning
            dangerous = ['gatetest', 'identify', 'test', 'scan',
                        'kick on', 'ramp off', 'defpwm',
                        'kickduty', 'kickms', 'ramprate', 'rampms']
            raw_lower = raw.lower()
            is_dangerous = any(raw_lower == d or raw_lower.startswith(d + ' ') for d in dangerous)
            if is_dangerous:
                if not messagebox.askyesno("Dangerous Command",
                    f"Sending dangerous command to F411:\n\n{raw}\n\n"
                    "This may drive the motor. Continue?"):
                    return
            self._send("m1 " + raw)

    def _send_raw_to_f446(self):
        raw = self.raw_var.get().strip()
        if raw:
            self._send(raw)

    def _start_heartbeat(self, cmd):
        self.heartbeat_cmd = cmd
        self.heartbeat_interval_ms = max(100, min(2000, safe_int(self.hb_var.get(), 300)))
        self.last_heartbeat_ms = int(time.time() * 1000)

    def _stop_heartbeat(self):
        self.heartbeat_cmd = None

    def _tick(self):
        try:
            now_ms = int(time.time() * 1000)

            # Telemetry age check
            if self._last_telem_ms > 0 and (now_ms - self._last_telem_ms) > 1000:
                if not self._telem_stale:
                    self._telem_stale = True
                    self._log("WARN|telemetry stale (>1s)")
                    self._draw_gauge(0, stale=True)

            if self.heartbeat_cmd and not self._telem_stale and now_ms - self.last_heartbeat_ms >= self.heartbeat_interval_ms:
                self.last_heartbeat_ms = now_ms
                self._send(self.heartbeat_cmd)

            while True:
                try:
                    line = self.client.rx_queue.get_nowait()
                except queue.Empty:
                    break
                self._handle_line(line)
        except Exception as exc:
            self._log(f"TICK ERR: {exc}")

        self.after(50, self._tick)

    def _handle_line(self, line):
        self._log("RX " + line)

        response_line = line
        if line.startswith("M1|"):
            payload = line[3:]
            response_line = payload
            fields = {}
            for part in payload.split(","):
                if ":" not in part:
                    continue
                k, v = part.split(":", 1)
                fields[k.strip()] = v.strip()

            if "Tcmd" in fields and "T" not in fields:
                fields["T"] = fields["Tcmd"]
            # APP_PH / PH backward compatibility
            if "APP_PH" in fields and "PH" not in fields:
                fields["PH"] = fields["APP_PH"]
            if "PH" in fields and "APP_PH" not in fields:
                fields["APP_PH"] = fields["PH"]

            for key, var in self.telemetry_vars.items():
                if key in fields:
                    var.set(fields[key])

            self._last_telem_ms = int(time.time() * 1000)
            self._telem_stale = False

            # Update RPM gauge
            try:
                rpm = int(fields.get("RPM", "0"))
            except (ValueError, TypeError):
                rpm = 0
            self._draw_gauge(rpm, stale=False)

        # Parse spstat response lines for PI tuning read-back
        m = re.match(r"Kp_m=([0-9.]+)\s+Ki_m=([0-9.]+)", response_line)
        if m:
            self.pi_kp.set(f"{int(m.group(1)) / 1000:.3f}")
            self.pi_ki.set(f"{int(m.group(2)) / 1000:.3f}")
            return
        m = re.match(r"Base\s+" + r"\s+".join([r"(\d+)"] * 8) + r"$", response_line)
        if m:
            for var, value in zip(self.pi_base, m.groups()):
                var.set(value)
            return
        m = re.match(r"Boost\s+" + r"\s+".join([r"(\d+)"] * 8) +
                     r"\s+ms=(\d+)$", response_line)
        if m:
            for var, value in zip(self.pi_boost, m.groups()[:8]):
                var.set(value)
            self.pi_boost_ms.set(m.group(9))
            return
        m = re.match(r"Ramp\s+up=([0-9.]+)\s+down=([0-9.]+)", response_line)
        if m:
            self.pi_ramp_up.set(m.group(1))
            self.pi_ramp_down.set(m.group(2))
            return

    def _log(self, line):
        ts = time.strftime("%H:%M:%S")
        self.console.insert("end", f"[{ts}] {line}\n")
        line_count = int(self.console.index("end-1c").split(".")[0])
        if line_count > 1000:
            self.console.delete("1.0", "200.0")
        self.console.see("end")

    def destroy(self):
        try:
            self._stop_heartbeat()
            if self.client.is_connected():
                try:
                    self.client.send("stop")
                except Exception:
                    pass
            self.client.disconnect()
        except Exception as exc:
            print(f"Cleanup error: {exc}", file=sys.stderr)
        super().destroy()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="F446 -> F411 Motor GUI")
    ap.add_argument("--port", default="", help="Default serial port")
    args = ap.parse_args()
    App(default_port=args.port).mainloop()
