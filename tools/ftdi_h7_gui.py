#!/usr/bin/env python3
"""
FTDI H7 GUI — Tkinter-based graphical interface for F411 motor controller.

Provides:
  - Connection management
  - Manual duty control (forward/backward/stop/brake)
  - Speed PI / RPM control with tuning
  - Service tools (hall, status, scan, test, identify, etc.)
  - Live telemetry dashboard
  - CSV telemetry logging
  - Raw serial console
  - Keyboard shortcuts (Space, Esc, F, B, S)

Requires:
    sudo pacman -S python-pyserial tk

Usage:
    python3 tools/ftdi_h7_gui.py
    python3 tools/ftdi_h7_gui.py --port /dev/ttyUSB0
"""

import argparse
import glob as globmod
import math
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox, scrolledtext, ttk

from ftdi_h7_client import DEFAULT_BAUD, DEFAULT_PORT, FtdiH7Client


def list_serial_ports():
    """Scan common serial port patterns across platforms."""
    import platform
    system = platform.system()
    if system == "Windows":
        ports = [f"COM{i}" for i in range(1, 33)]
    elif system == "Darwin":
        ports = globmod.glob("/dev/cu.*") + globmod.glob("/dev/tty.*")
    else:
        ports = globmod.glob("/dev/ttyUSB*") + globmod.glob("/dev/ttyACM*")
    return sorted(ports)

# ── Colors (Catppuccin Mocha) ─────────────────────────────────────
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
    "peach": "#fab387",
}

TICK_MS = 100  # UI update interval
HEARTBEAT_DEFAULT = 0.4  # seconds
GUI_RPM_GAUGE_MAX = 500  # max RPM for gauge scale


class FtdiH7Gui(tk.Tk):
    def __init__(self, port, baud):
        super().__init__()
        self.title("FTDI H7 Motor Controller")
        self.geometry("1100x780")
        self.configure(bg=C["bg"])
        self.minsize(900, 600)

        self.client = FtdiH7Client(port, baud)
        self._port = port
        self._baud = baud
        self._connected = False

        # Motion state
        self._motion_active = False
        self._motion_cmd = None  # 'f', 'b', 'rpm <val>'
        self._motion_heartbeat_cmd = None

        # Service state
        self._service_running = False

        self._build_ui()
        self._bind_keys()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self._tick()

    # ═══════════════════════════════════════════════════════════════
    #  UI Construction
    # ═══════════════════════════════════════════════════════════════

    def _build_ui(self):
        # Top connection bar
        self._build_top_bar()

        # Main body with notebook tabs
        body = tk.Frame(self, bg=C["bg"])
        body.pack(fill="both", expand=True, padx=5, pady=(0, 0))

        # Left: notebook tabs
        left = tk.Frame(body, bg=C["bg"])
        left.pack(side="left", fill="both", expand=True)

        self.notebook = tk.Frame(left, bg=C["bg"])
        self.notebook.pack(fill="both", expand=True)

        # Tab buttons
        tab_bar = tk.Frame(left, bg=C["surface"])
        tab_bar.pack(fill="x")
        self._tabs = {}
        self._tab_frames = {}
        tab_names = ["Duty Control", "Speed PI", "Service", "Logging"]
        for i, name in enumerate(tab_names):
            b = tk.Button(
                tab_bar, text=name, font=("monospace", 9, "bold"),
                bg=C["surface"], fg=C["dim"], relief="flat", padx=10, pady=4,
                command=lambda n=name: self._show_tab(n),
                cursor="hand2",
            )
            b.pack(side="left", padx=1)
            self._tabs[name] = b

        for name in tab_names:
            fr = tk.Frame(self.notebook, bg=C["bg"])
            self._tab_frames[name] = fr

        self._build_duty_tab(self._tab_frames["Duty Control"])
        self._build_speed_tab(self._tab_frames["Speed PI"])
        self._build_service_tab(self._tab_frames["Service"])
        self._build_logging_tab(self._tab_frames["Logging"])
        self._show_tab("Duty Control")

        # Right: telemetry + console
        right = tk.Frame(body, bg=C["bg"], width=340)
        right.pack(side="right", fill="y", padx=(5, 0))
        right.pack_propagate(False)

        self._build_telemetry_panel(right)
        self._build_console_panel(right)

        # Bottom status bar
        self._build_status_bar()

    # ── Top bar ─────────────────────────────────────────────────────

    def _build_top_bar(self):
        bar = tk.Frame(self, bg=C["surface"], height=36)
        bar.pack(fill="x")
        bar.pack_propagate(False)

        tk.Label(bar, text="Port:", bg=C["surface"], fg=C["dim"],
                 font=("monospace", 9)).pack(side="left", padx=(8, 2))

        self._port_options = list_serial_ports()
        if self._port not in self._port_options and self._port:
            self._port_options.insert(0, self._port)
        elif not self._port_options:
            self._port_options = [self._port]

        self.port_var = tk.StringVar(value=self._port)
        self.port_combo = ttk.Combobox(
            bar, textvariable=self.port_var,
            values=self._port_options, width=14, state="readonly",
        )
        self.port_combo.pack(side="left", padx=2)
        # Style the combobox to match dark theme
        bar.option_add("*TCombobox*Listbox.background", C["surface"])
        bar.option_add("*TCombobox*Listbox.foreground", C["fg"])

        tk.Button(bar, text="↻", bg=C["overlay"], fg=C["fg"],
                  font=("monospace", 9, "bold"), relief="flat", padx=4,
                  command=self._refresh_ports, cursor="hand2").pack(side="left", padx=2)

        tk.Label(bar, text="Baud:", bg=C["surface"], fg=C["dim"],
                 font=("monospace", 9)).pack(side="left", padx=(8, 2))
        self.baud_entry = tk.Entry(bar, bg=C["overlay"], fg=C["fg"],
                                   font=("monospace", 9), width=8,
                                   insertbackground=C["fg"], relief="flat")
        self.baud_entry.insert(0, str(self._baud))
        self.baud_entry.pack(side="left", padx=2)

        self.connect_btn = tk.Button(
            bar, text="Connect", bg=C["blue"], fg=C["bg"],
            font=("monospace", 9, "bold"), relief="flat", padx=8,
            command=self._do_connect, cursor="hand2",
        )
        self.connect_btn.pack(side="left", padx=6)

        self.disconnect_btn = tk.Button(
            bar, text="Disconnect", bg=C["overlay"], fg=C["dim"],
            font=("monospace", 9), relief="flat", padx=8,
            command=self._do_disconnect, state="disabled", cursor="hand2",
        )
        self.disconnect_btn.pack(side="left", padx=2)

        self.conn_status = tk.Label(
            bar, text=" DISCONNECTED ", bg=C["red"], fg=C["bg"],
            font=("monospace", 9, "bold"), padx=8,
        )
        self.conn_status.pack(side="left", padx=10)

        # Mode buttons — canonical cube names (ISSUE-037).  The cube
        # firmware also accepts the legacy `mode normal` / `mode control`
        # aliases, but the labels here use the canonical names.  The
        # legacy firmware had three modes (Normal/Control/Settings);
        # the cube firmware has two (Duty/Speed), so the third button
        # is dropped.
        for label, cmd in [("Duty", "mode duty"), ("Speed", "mode speed")]:
            tk.Button(
                bar, text=label, bg=C["overlay"], fg=C["fg"],
                font=("monospace", 8), relief="flat", padx=6,
                command=lambda c=cmd: self._send(c), cursor="hand2",
            ).pack(side="left", padx=2)

        # Emergency stop
        self.estop_btn = tk.Button(
            bar, text="E-STOP", bg=C["red"], fg=C["bg"],
            font=("monospace", 10, "bold"), relief="flat", padx=12,
            command=self._emergency_stop, cursor="hand2",
        )
        self.estop_btn.pack(side="right", padx=8)

    # ── Duty Control tab ────────────────────────────────────────────

    def _build_duty_tab(self, parent):
        parent.pack(fill="both", expand=True)
        r = 0

        self._section_label(parent, "Manual Duty Control", r); r += 1

        # PWM slider + entry
        fr = tk.Frame(parent, bg=C["bg"])
        fr.grid(row=r, column=0, sticky="ew", padx=10, pady=4)
        parent.columnconfigure(0, weight=1)

        tk.Label(fr, text="PWM:", bg=C["bg"], fg=C["fg"],
                 font=("monospace", 10)).pack(side="left")
        self.pwm_slider = tk.Scale(
            fr, from_=0, to=4000, orient="horizontal",
            bg=C["bg"], fg=C["fg"], troughcolor=C["surface"],
            activebackground=C["cyan"], highlightthickness=0,
            showvalue=False, length=220, sliderlength=20,
            command=self._on_pwm_slider,
        )
        self.pwm_slider.set(2400)
        self.pwm_slider.pack(side="left", padx=6)

        self.pwm_entry = tk.Entry(fr, bg=C["surface"], fg=C["fg"],
                                  font=("monospace", 10), width=5,
                                  insertbackground=C["fg"], relief="flat")
        self.pwm_entry.insert(0, "2400")
        self.pwm_entry.pack(side="left", padx=4)

        tk.Button(fr, text="Apply", bg=C["blue"], fg=C["bg"],
                  font=("monospace", 9, "bold"), relief="flat",
                  command=self._apply_pwm, cursor="hand2",
                  padx=6).pack(side="left", padx=4)
        r += 1

        # Direction buttons
        fr2 = tk.Frame(parent, bg=C["bg"])
        fr2.grid(row=r, column=0, sticky="w", padx=10, pady=6)

        self.fwd_btn = tk.Button(
            fr2, text="Forward [F]", bg=C["green"], fg=C["bg"],
            font=("monospace", 10, "bold"), relief="flat", padx=12, pady=6,
            command=self._go_forward, cursor="hand2",
        )
        self.fwd_btn.pack(side="left", padx=4)

        self.bwd_btn = tk.Button(
            fr2, text="Backward [B]", bg=C["peach"], fg=C["bg"],
            font=("monospace", 10, "bold"), relief="flat", padx=12, pady=6,
            command=self._go_backward, cursor="hand2",
        )
        self.bwd_btn.pack(side="left", padx=4)

        self.stop_btn = tk.Button(
            fr2, text="Stop [S/Space]", bg=C["yellow"], fg=C["bg"],
            font=("monospace", 10, "bold"), relief="flat", padx=12, pady=6,
            command=self._do_stop, cursor="hand2",
        )
        self.stop_btn.pack(side="left", padx=4)

        self.brake_btn = tk.Button(
            fr2, text="Brake", bg=C["magenta"], fg=C["bg"],
            font=("monospace", 10, "bold"), relief="flat", padx=12, pady=6,
            command=self._do_brake, cursor="hand2",
        )
        self.brake_btn.pack(side="left", padx=4)
        r += 1

        # Heartbeat interval
        fr3 = tk.Frame(parent, bg=C["bg"])
        fr3.grid(row=r, column=0, sticky="w", padx=10, pady=4)
        tk.Label(fr3, text="Heartbeat (s):", bg=C["bg"], fg=C["dim"],
                 font=("monospace", 9)).pack(side="left")
        self.hb_entry = tk.Entry(fr3, bg=C["surface"], fg=C["fg"],
                                 font=("monospace", 9), width=5,
                                 insertbackground=C["fg"], relief="flat")
        self.hb_entry.insert(0, str(HEARTBEAT_DEFAULT))
        self.hb_entry.pack(side="left", padx=4)
        r += 1

        # Info
        tk.Label(parent, text="While moving, f/b is repeated as heartbeat.",
                 bg=C["bg"], fg=C["dim"], font=("monospace", 8)).grid(
            row=r, column=0, sticky="w", padx=12)

    # ── Speed PI tab ────────────────────────────────────────────────

    def _build_speed_tab(self, parent):
        parent.pack(fill="both", expand=True)
        r = 0

        self._section_label(parent, "RPM Target", r); r += 1

        fr = tk.Frame(parent, bg=C["bg"])
        fr.grid(row=r, column=0, sticky="w", padx=10, pady=4)

        tk.Label(fr, text="RPM:", bg=C["bg"], fg=C["fg"],
                 font=("monospace", 10)).pack(side="left")
        self.rpm_entry = tk.Entry(fr, bg=C["surface"], fg=C["fg"],
                                  font=("monospace", 10), width=8,
                                  insertbackground=C["fg"], relief="flat")
        self.rpm_entry.insert(0, "100")
        self.rpm_entry.pack(side="left", padx=4)

        tk.Button(fr, text="Set RPM", bg=C["green"], fg=C["bg"],
                  font=("monospace", 9, "bold"), relief="flat",
                  command=self._set_rpm, cursor="hand2",
                  padx=6).pack(side="left", padx=4)

        tk.Button(fr, text="Stop RPM", bg=C["red"], fg=C["bg"],
                  font=("monospace", 9, "bold"), relief="flat",
                  command=self._stop_rpm, cursor="hand2",
                  padx=6).pack(side="left", padx=4)
        r += 1

        # Quick RPM buttons
        fr2 = tk.Frame(parent, bg=C["bg"])
        fr2.grid(row=r, column=0, sticky="w", padx=10, pady=4)
        for rpm_val in [10, 15, 23, 50, 100, 300, 400]:
            tk.Button(
                fr2, text=str(rpm_val), bg=C["overlay"], fg=C["fg"],
                font=("monospace", 9), relief="flat", padx=6,
                command=lambda v=rpm_val: self._quick_rpm(v),
                cursor="hand2",
            ).pack(side="left", padx=2)
        r += 1

        # PI tuning section
        self._section_label(parent, "PI Tuning", r); r += 1

        pi_fields = [
            ("Kp:", "kp", "0.80"),
            ("Ki:", "ki", "0.10"),
            ("Base Lo:", "base_lo", "55"),
            ("Base Mid:", "base_mid", "45"),
            ("Base Hi:", "base_hi", "35"),
            ("Boost Lo:", "boost_lo", "65"),
            ("Boost Mid:", "boost_mid", "65"),
            ("Boost Hi:", "boost_hi", "65"),
            ("Boost ms:", "boost_ms", "150"),
            ("Ramp Up:", "ramp_up", "100"),
            ("Ramp Down:", "ramp_down", "200"),
        ]
        self._pi_entries = {}
        grid_fr = tk.Frame(parent, bg=C["bg"])
        grid_fr.grid(row=r, column=0, sticky="w", padx=10, pady=4)
        for i, (label, key, default) in enumerate(pi_fields):
            row_i = i // 3
            col_i = (i % 3) * 2
            tk.Label(grid_fr, text=label, bg=C["bg"], fg=C["fg"],
                     font=("monospace", 9), width=9, anchor="w").grid(
                row=row_i, column=col_i, sticky="w", padx=2, pady=1)
            ent = tk.Entry(grid_fr, bg=C["surface"], fg=C["fg"],
                           font=("monospace", 9), width=7,
                           insertbackground=C["fg"], relief="flat")
            ent.insert(0, default)
            ent.grid(row=row_i, column=col_i + 1, padx=2, pady=1)
            self._pi_entries[key] = ent
        r += 1

        fr3 = tk.Frame(parent, bg=C["bg"])
        fr3.grid(row=r, column=0, sticky="w", padx=10, pady=6)

        tk.Button(fr3, text="Apply PI Settings", bg=C["blue"], fg=C["bg"],
                  font=("monospace", 9, "bold"), relief="flat",
                  command=self._apply_pi_settings, cursor="hand2",
                  padx=8).pack(side="left", padx=4)

        tk.Button(fr3, text="spstat", bg=C["overlay"], fg=C["fg"],
                  font=("monospace", 9), relief="flat",
                  command=lambda: self._send("spstat"),
                  cursor="hand2", padx=6).pack(side="left", padx=4)

    # ── Service tab ─────────────────────────────────────────────────

    def _build_service_tab(self, parent):
        parent.pack(fill="both", expand=True)
        r = 0

        self._section_label(parent, "Service Commands", r); r += 1

        # Simple commands
        simple_cmds = [
            ("Hall", "hall"), ("Status", "status"), ("Spstat", "spstat"),
            ("Map", "map"), ("Map Reset", "mapreset"), ("Reload", "reload"),
            ("Save", "save"), ("Clear Fault", "clrerr"),
        ]
        fr = tk.Frame(parent, bg=C["bg"])
        fr.grid(row=r, column=0, sticky="w", padx=10, pady=4)
        for i, (label, cmd) in enumerate(simple_cmds):
            tk.Button(
                fr, text=label, bg=C["overlay"], fg=C["fg"],
                font=("monospace", 9), relief="flat", padx=8, pady=2,
                command=lambda c=cmd: self._service_cmd(c),
                cursor="hand2",
            ).grid(row=i // 4, column=i % 4, padx=3, pady=3)
        r += 1

        self._section_label(parent, "Long-running Tasks", r); r += 1

        fr2 = tk.Frame(parent, bg=C["bg"])
        fr2.grid(row=r, column=0, sticky="w", padx=10, pady=4)

        tk.Button(fr2, text="Scan", bg=C["cyan"], fg=C["bg"],
                  font=("monospace", 10, "bold"), relief="flat", padx=10,
                  command=lambda: self._service_task("scan"),
                  cursor="hand2").pack(side="left", padx=4)

        tk.Button(fr2, text="Test", bg=C["yellow"], fg=C["bg"],
                  font=("monospace", 10, "bold"), relief="flat", padx=10,
                  command=lambda: self._service_task("test"),
                  cursor="hand2").pack(side="left", padx=4)

        tk.Button(fr2, text="Identify", bg=C["magenta"], fg=C["bg"],
                  font=("monospace", 10, "bold"), relief="flat", padx=10,
                  command=lambda: self._service_task("identify"),
                  cursor="hand2").pack(side="left", padx=4)

        tk.Button(fr2, text="Identify + Save", bg=C["peach"], fg=C["bg"],
                  font=("monospace", 10, "bold"), relief="flat", padx=10,
                  command=lambda: self._service_task("identify_save"),
                  cursor="hand2").pack(side="left", padx=4)
        r += 1

        tk.Label(parent, text="Service commands auto-stop motion first.",
                 bg=C["bg"], fg=C["dim"], font=("monospace", 8)).grid(
            row=r, column=0, sticky="w", padx=12)

    # ── Logging tab ─────────────────────────────────────────────────

    def _build_logging_tab(self, parent):
        parent.pack(fill="both", expand=True)
        r = 0

        self._section_label(parent, "CSV Telemetry Logging", r); r += 1

        fr = tk.Frame(parent, bg=C["bg"])
        fr.grid(row=r, column=0, sticky="w", padx=10, pady=6)

        self.log_start_btn = tk.Button(
            fr, text="Start CSV Log", bg=C["green"], fg=C["bg"],
            font=("monospace", 10, "bold"), relief="flat", padx=10,
            command=self._start_csv_log, cursor="hand2",
        )
        self.log_start_btn.pack(side="left", padx=4)

        self.log_stop_btn = tk.Button(
            fr, text="Stop CSV Log", bg=C["red"], fg=C["bg"],
            font=("monospace", 10, "bold"), relief="flat", padx=10,
            command=self._stop_csv_log, state="disabled", cursor="hand2",
        )
        self.log_stop_btn.pack(side="left", padx=4)
        r += 1

        self.log_path_lbl = tk.Label(
            parent, text="Not logging", bg=C["bg"], fg=C["dim"],
            font=("monospace", 9), anchor="w",
        )
        self.log_path_lbl.grid(row=r, column=0, sticky="w", padx=12, pady=4)

    # ── Telemetry panel (right side) ────────────────────────────────

    def _build_telemetry_panel(self, parent):
        self._section_label(parent, "Live Telemetry", 0)

        fields = [
            ("RPM:", "rpm_val", C["green"]),
            ("Target:", "target_val", C["cyan"]),
            ("Duty:", "duty_val", C["fg"]),
            ("Direction:", "dir_val", C["yellow"]),
            ("Phase:", "phase_val", C["cyan"]),
            ("Speed Mode:", "sp_val", C["fg"]),
            ("Brake:", "brake_val", C["magenta"]),
            ("Fault:", "fault_val", C["red"]),
            ("Hall:", "hall_val", C["fg"]),
            ("PWM Set:", "pwm_set_val", C["fg"]),
            ("PWM Act:", "pwm_act_val", C["yellow"]),
        ]
        self._telem_labels = {}
        for i, (label, key, color) in enumerate(fields):
            row = i + 1
            tk.Label(parent, text=label, bg=C["bg"], fg=C["dim"],
                     font=("monospace", 9), anchor="w").grid(
                row=row, column=0, sticky="w", padx=8, pady=1)
            lbl = tk.Label(parent, text="--", bg=C["bg"], fg=color,
                           font=("monospace", 10, "bold"), anchor="w")
            lbl.grid(row=row, column=1, sticky="w", padx=4, pady=1)
            self._telem_labels[key] = lbl

        last_row = len(fields) + 1
        tk.Label(parent, text="Last raw:", bg=C["bg"], fg=C["dim"],
                 font=("monospace", 8), anchor="w").grid(
            row=last_row, column=0, columnspan=2, sticky="w", padx=8, pady=(6, 0))
        self.raw_telem_lbl = tk.Label(
            parent, text="--", bg=C["surface"], fg=C["dim"],
            font=("monospace", 7), anchor="w", wraplength=310, justify="left",
        )
        self.raw_telem_lbl.grid(row=last_row + 1, column=0, columnspan=2,
                                sticky="ew", padx=8, pady=2)

        # RPM Gauge
        gauge_row = last_row + 2
        tk.Label(parent, text="── RPM Gauge ──", bg=C["bg"], fg=C["cyan"],
                 font=("monospace", 11, "bold"), anchor="w").grid(
            row=gauge_row, column=0, columnspan=2, sticky="w", padx=10, pady=(10, 2))
        self._build_rpm_gauge(parent, row=gauge_row + 1)

    # ── Console panel (right side) ──────────────────────────────────

    def _build_console_panel(self, parent):
        console_fr = tk.Frame(parent, bg=C["bg"])
        console_fr.pack(fill="both", expand=True, pady=(8, 0))

        tk.Label(console_fr, text="Console", bg=C["bg"], fg=C["dim"],
                 font=("monospace", 9, "bold")).pack(anchor="w", padx=8)

        self.console_text = scrolledtext.ScrolledText(
            console_fr, bg=C["surface"], fg=C["fg"],
            font=("monospace", 8), height=10, width=40,
            state="disabled", wrap="word", relief="flat",
        )
        self.console_text.pack(fill="both", expand=True, padx=8, pady=2)

        # Raw command entry
        cmd_fr = tk.Frame(console_fr, bg=C["bg"])
        cmd_fr.pack(fill="x", padx=8, pady=2)

        self.raw_entry = tk.Entry(
            cmd_fr, bg=C["surface"], fg=C["fg"],
            font=("monospace", 9), insertbackground=C["fg"], relief="flat",
        )
        self.raw_entry.pack(side="left", fill="x", expand=True, padx=(0, 4))
        self.raw_entry.bind("<Return>", lambda e: self._send_raw())

        tk.Button(cmd_fr, text="Send", bg=C["blue"], fg=C["bg"],
                  font=("monospace", 9, "bold"), relief="flat",
                  command=self._send_raw, cursor="hand2",
                  padx=6).pack(side="right")

    # ── RPM Gauge (right side) ─────────────────────────────────────

    def _build_rpm_gauge(self, parent, row=0):
        """Canvas-based semicircular RPM gauge with needle."""
        self._gauge_size = 160
        self._gauge_canvas = tk.Canvas(
            parent,
            width=self._gauge_size,
            height=self._gauge_size // 2 + 25,
            bg=C["bg"], highlightthickness=0,
        )
        self._gauge_canvas.grid(row=row, column=0, columnspan=2,
                                padx=8, pady=4)
        self._gauge_stale_lbl = tk.Label(
            parent, text="", bg=C["bg"], fg=C["dim"],
            font=("monospace", 8),
        )
        self._gauge_stale_lbl.grid(row=row + 1, column=0, columnspan=2)
        self._draw_gauge(0, stale=True)

    def _draw_gauge(self, rpm, stale=False):
        """Draw the semicircular gauge with needle at the given RPM."""
        c = self._gauge_canvas
        c.delete("all")
        w = self._gauge_size
        h = self._gauge_size // 2 + 25
        cx = w // 2
        cy = h - 8
        r = w // 2 - 12

        # Arc background
        c.create_arc(cx - r, cy - r, cx + r, cy + r,
                     start=0, extent=180, style="arc",
                     outline=C["overlay"], width=4)

        # Tick marks and labels
        max_rpm = GUI_RPM_GAUGE_MAX
        for i in range(11):
            angle_deg = 180 - i * 18
            angle_rad = math.radians(angle_deg)
            x1 = cx + (r - 8) * math.cos(angle_rad)
            y1 = cy - (r - 8) * math.sin(angle_rad)
            x2 = cx + (r + 2) * math.cos(angle_rad)
            y2 = cy - (r + 2) * math.sin(angle_rad)
            c.create_line(x1, y1, x2, y2, fill=C["dim"], width=2)
            if i % 2 == 0:
                val = int(max_rpm * i / 10)
                lx = cx + (r - 20) * math.cos(angle_rad)
                ly = cy - (r - 20) * math.sin(angle_rad)
                c.create_text(lx, ly, text=str(val), fill=C["fg"],
                              font=("monospace", 7))

        # Needle color
        if stale:
            needle_color = C["dim"]
        else:
            needle_color = C["red"] if rpm >= max_rpm else C["green"]

        rpm_clamped = max(0, min(rpm, max_rpm))
        needle_angle = 180 - (rpm_clamped / max_rpm) * 180
        needle_rad = math.radians(needle_angle)
        nx = cx + (r - 5) * math.cos(needle_rad)
        ny = cy - (r - 5) * math.sin(needle_rad)
        c.create_line(cx, cy, nx, ny, fill=needle_color, width=3,
                      capstyle="round")

        # Center dot
        c.create_oval(cx - 4, cy - 4, cx + 4, cy + 4, fill=C["fg"])

        # RPM value
        c.create_text(cx, cy - 14, text=f"{rpm:.0f}",
                      fill=C["yellow"], font=("monospace", 13, "bold"))
        c.create_text(cx, cy, text="RPM",
                      fill=C["dim"], font=("monospace", 7))

        # Stale indicator
        if stale:
            self._gauge_stale_lbl.configure(text="STALE (no telemetry)")
        else:
            self._gauge_stale_lbl.configure(text="")

    # ── Status bar ──────────────────────────────────────────────────

    def _build_status_bar(self):
        bar = tk.Frame(self, bg=C["surface"], height=22)
        bar.pack(fill="x", side="bottom")
        bar.pack_propagate(False)

        self.status_lbl = tk.Label(
            bar, text="Disconnected", bg=C["surface"], fg=C["dim"],
            font=("monospace", 8),
        )
        self.status_lbl.pack(side="left", padx=8)

        tk.Label(bar, text="Esc: E-STOP | Space/S: Stop | F: Fwd | B: Bwd",
                 bg=C["surface"], fg=C["dim"],
                 font=("monospace", 8)).pack(side="right", padx=8)

    # ═══════════════════════════════════════════════════════════════
    #  Helpers
    # ═══════════════════════════════════════════════════════════════

    def _section_label(self, parent, text, row):
        tk.Label(parent, text=f"── {text} ──", bg=C["bg"], fg=C["cyan"],
                 font=("monospace", 11, "bold"), anchor="w").grid(
            row=row, column=0, sticky="w", padx=10, pady=(8, 2))

    def _show_tab(self, name):
        for n, fr in self._tab_frames.items():
            fr.pack_forget()
        self._tab_frames[name].pack(fill="both", expand=True)
        for n, b in self._tabs.items():
            if n == name:
                b.configure(bg=C["blue"], fg=C["bg"])
            else:
                b.configure(bg=C["surface"], fg=C["dim"])

    def _send(self, cmd):
        if self._connected:
            self.client.send(cmd)
            self._console_log(f">> {cmd}")

    def _console_log(self, text):
        self.console_text.configure(state="normal")
        self.console_text.insert("end", text + "\n")
        self.console_text.see("end")
        self.console_text.configure(state="disabled")

    def _get_heartbeat_interval(self):
        try:
            return max(0.2, float(self.hb_entry.get()))
        except ValueError:
            return HEARTBEAT_DEFAULT

    # ═══════════════════════════════════════════════════════════════
    #  Connection
    # ═══════════════════════════════════════════════════════════════

    def _do_connect(self):
        if self._connected:
            return
        port = self.port_var.get().strip()
        if not port:
            messagebox.showerror("Error", "No port selected")
            return
        try:
            baud = int(self.baud_entry.get().strip())
        except ValueError:
            messagebox.showerror("Error", "Invalid baud rate")
            return
        self.client.port = port
        self.client.baud = baud
        try:
            self.client.connect()
        except Exception as e:
            messagebox.showerror("Connection Failed", str(e))
            return
        self._connected = True
        self._port = port
        self._baud = baud
        self.connect_btn.configure(state="disabled")
        self.disconnect_btn.configure(state="normal")
        self.port_combo.configure(state="disabled")
        self.conn_status.configure(text=" CONNECTED ", bg=C["green"], fg=C["bg"])
        self._console_log(f"Connected to {port} @ {baud}")

    def _do_disconnect(self):
        if not self._connected:
            return
        self._stop_all_motion()
        self.client.safe_stop()
        time.sleep(0.05)
        self.client.disconnect()
        self._connected = False
        self.connect_btn.configure(state="normal")
        self.disconnect_btn.configure(state="disabled")
        self.port_combo.configure(state="readonly")
        self.conn_status.configure(text=" DISCONNECTED ", bg=C["red"], fg=C["bg"])
        self._console_log("Disconnected")

    def _refresh_ports(self):
        if self._connected:
            return
        self._port_options = list_serial_ports()
        if not self._port_options:
            self._port_options = [DEFAULT_PORT]
        current = self.port_var.get()
        self.port_combo.configure(values=self._port_options)
        if current not in self._port_options:
            self.port_var.set(self._port_options[0])

    # ═══════════════════════════════════════════════════════════════
    #  Duty Control
    # ═══════════════════════════════════════════════════════════════

    def _on_pwm_slider(self, val):
        try:
            v = int(val)
            self.pwm_entry.delete(0, "end")
            self.pwm_entry.insert(0, str(v))
        except ValueError:
            pass

    def _apply_pwm(self):
        try:
            v = int(self.pwm_entry.get())
            v = max(0, min(4000, v))
        except ValueError:
            return
        self._send(f"pwm {v}")
        self.pwm_slider.set(v)

    def _get_pwm_value(self):
        try:
            v = int(self.pwm_entry.get().strip())
        except ValueError:
            self._console_log("[ERR] Invalid PWM value")
            return None
        v = max(0, min(4000, v))
        self.pwm_entry.delete(0, "end")
        self.pwm_entry.insert(0, str(v))
        self.pwm_slider.set(v)
        return v

    def _go_forward(self):
        if not self._connected:
            return
        pwm = self._get_pwm_value()
        if pwm is None:
            return
        self._stop_all_motion()
        self._send("mode duty")
        self._send(f"f{pwm}")
        hb = self._get_heartbeat_interval()
        self.client.set_heartbeat(f"f{pwm}", interval=hb)
        self._motion_active = True
        self._motion_cmd = f"f{pwm}"
        self._console_log(f"Forward PWM={pwm}")

    def _go_backward(self):
        if not self._connected:
            return
        pwm = self._get_pwm_value()
        if pwm is None:
            return
        self._stop_all_motion()
        self._send("mode duty")
        self._send(f"b{pwm}")
        hb = self._get_heartbeat_interval()
        self.client.set_heartbeat(f"b{pwm}", interval=hb)
        self._motion_active = True
        self._motion_cmd = f"b{pwm}"
        self._console_log(f"Backward PWM={pwm}")

    def _do_stop(self):
        self._stop_all_motion()
        self._send("s")
        self._console_log("Stop (coast)")

    def _do_brake(self):
        self._stop_all_motion()
        self._send("x")
        self._console_log("Brake")

    def _stop_all_motion(self):
        self._motion_active = False
        self._motion_cmd = None
        self.client.clear_heartbeat()

    # ═══════════════════════════════════════════════════════════════
    #  Speed PI / RPM
    # ═══════════════════════════════════════════════════════════════

    def _set_rpm(self):
        if not self._connected:
            return
        try:
            rpm = int(self.rpm_entry.get())
        except ValueError:
            return
        self._stop_all_motion()
        self._send("mode speed")
        self._send(f"rpm {rpm}")
        hb = self._get_heartbeat_interval()
        self.client.set_heartbeat(f"rpm {rpm}", interval=hb)
        self._motion_active = True
        self._motion_cmd = f"rpm {rpm}"
        self._console_log(f"Speed RPM={rpm}")

    def _quick_rpm(self, rpm):
        self.rpm_entry.delete(0, "end")
        self.rpm_entry.insert(0, str(rpm))
        self._set_rpm()

    def _stop_rpm(self):
        self._stop_all_motion()
        self._send("rpm 0")
        self._send("s")
        self._console_log("RPM stopped")

    def _apply_pi_settings(self):
        e = self._pi_entries
        cmds = []
        kp = e["kp"].get().strip()
        ki = e["ki"].get().strip()
        if kp and ki:
            cmds.append(f"pi {kp} {ki}")

        lo = e["base_lo"].get().strip()
        mid = e["base_mid"].get().strip()
        hi = e["base_hi"].get().strip()
        if lo and mid and hi:
            cmds.append(f"base {lo} {mid} {hi}")

        blo = e["boost_lo"].get().strip()
        bmid = e["boost_mid"].get().strip()
        bhi = e["boost_hi"].get().strip()
        bms = e["boost_ms"].get().strip()
        if blo and bmid and bhi and bms:
            cmds.append(f"boost {blo} {bmid} {bhi} {bms}")

        rup = e["ramp_up"].get().strip()
        rdown = e["ramp_down"].get().strip()
        if rup and rdown:
            cmds.append(f"ramp {rup} {rdown}")

        for cmd in cmds:
            self._send(cmd)
        self._console_log(f"PI settings applied ({len(cmds)} commands)")

    # ═══════════════════════════════════════════════════════════════
    #  Service Commands
    # ═══════════════════════════════════════════════════════════════

    def _service_cmd(self, cmd):
        if not self._connected:
            return
        self._stop_all_motion()
        self._send(cmd)
        self._console_log(f"Service: {cmd}")

    def _service_task(self, task):
        if not self._connected or self._service_running:
            return
        self._stop_all_motion()
        self._send("s")
        self._service_running = True

        def run_task():
            try:
                if task == "identify_save":
                    self._send("identify")
                    self.client.drain_until_pattern(
                        30.0,
                        patterns=["[OK]", "[ERR]", "[FAIL]", "Done", "done"],
                    )
                    time.sleep(0.3)
                    self._send("save")
                    time.sleep(1.0)
                    self.client.drain_all()
                else:
                    self._send(task)
                    timeout_map = {"scan": 15.0, "test": 20.0, "identify": 30.0}
                    timeout = timeout_map.get(task, 10.0)
                    self.client.drain_until_timeout(timeout)
            except Exception as exc:
                self.after(0, lambda: self._console_log(f"[ERR] Service task error: {exc}"))
            finally:
                self._service_running = False
                self.after(0, lambda: self._console_log(f"Service task '{task}' done"))

        threading.Thread(target=run_task, daemon=True).start()
        self._console_log(f"Service task started: {task}")

    # ═══════════════════════════════════════════════════════════════
    #  Logging
    # ═══════════════════════════════════════════════════════════════

    def _start_csv_log(self):
        path = self.client.start_csv_log()
        if path:
            self.log_path_lbl.configure(text=f"Logging: {path}", fg=C["green"])
            self.log_start_btn.configure(state="disabled")
            self.log_stop_btn.configure(state="normal")
            self._console_log(f"CSV log started: {path}")

    def _stop_csv_log(self):
        self.client.stop_csv_log()
        self.log_path_lbl.configure(text="Not logging", fg=C["dim"])
        self.log_start_btn.configure(state="normal")
        self.log_stop_btn.configure(state="disabled")
        self._console_log("CSV log stopped")

    # ═══════════════════════════════════════════════════════════════
    #  Raw Console
    # ═══════════════════════════════════════════════════════════════

    def _send_raw(self):
        cmd = self.raw_entry.get().strip()
        if cmd and self._connected:
            self._send(cmd)
            self.raw_entry.delete(0, "end")

    # ═══════════════════════════════════════════════════════════════
    #  Emergency Stop
    # ═══════════════════════════════════════════════════════════════

    def _emergency_stop(self):
        self._stop_all_motion()
        if self._connected:
            self.client.safe_stop()
        self._console_log("EMERGENCY STOP")

    # ═══════════════════════════════════════════════════════════════
    #  Keyboard Shortcuts
    # ═══════════════════════════════════════════════════════════════

    def _bind_keys(self):
        def _guard(func):
            def wrapper(e):
                w = self.focus_get()
                if w and isinstance(w, (tk.Entry, ttk.Entry, tk.Text, scrolledtext.ScrolledText)):
                    return
                func()
            return wrapper
        self.bind("<Escape>", _guard(self._emergency_stop))
        self.bind("<space>", _guard(self._do_stop))
        self.bind("<KeyPress-f>", _guard(self._go_forward))
        self.bind("<KeyPress-F>", _guard(self._go_forward))
        self.bind("<KeyPress-b>", _guard(self._go_backward))
        self.bind("<KeyPress-B>", _guard(self._go_backward))
        self.bind("<KeyPress-s>", _guard(self._do_stop))
        self.bind("<KeyPress-S>", _guard(self._do_stop))

    # ═══════════════════════════════════════════════════════════════
    #  UI Tick — periodic telemetry sync
    # ═══════════════════════════════════════════════════════════════

    def _tick(self):
        # Drain console lines from background threads
        try:
            while True:
                line = self.client.all_lines_queue.get_nowait()
                if not line.startswith("RPM:"):
                    self._console_log(line)
        except queue.Empty:
            pass
        except Exception as exc:
            self._console_log(f"[WARN] _tick error: {exc}")

        # Update telemetry display
        d = self.client.latest_display
        if d:
            self._telem_labels["rpm_val"].configure(text=str(d.get("rpm", 0)))
            self._telem_labels["target_val"].configure(
                text=str(d.get("pwm_set", 0)))
            self._telem_labels["duty_val"].configure(text=str(d.get("duty", 0)))
            self._telem_labels["dir_val"].configure(text=d.get("dir_str", "-"))
            self._telem_labels["phase_val"].configure(
                text=d.get("phase_name", "?"))
            self._telem_labels["sp_val"].configure(
                text="YES" if d.get("speed_mode") else "NO")
            self._telem_labels["brake_val"].configure(
                text="YES" if d.get("brake") else "NO")
            fc = d.get("fault_code", 0)
            self._telem_labels["fault_val"].configure(
                text=str(fc), fg=C["red"] if fc else C["green"])
            self._telem_labels["hall_val"].configure(text=str(d.get("hall", 0)))
            self._telem_labels["pwm_set_val"].configure(
                text=str(d.get("pwm_set", 0)))
            self._telem_labels["pwm_act_val"].configure(
                text=str(d.get("pwm_act", 0)))

            # Update RPM gauge
            rpm = d.get("rpm", 0)
            self._draw_gauge(rpm, stale=False)

        raw = self.client.latest_raw_line
        if raw:
            self.raw_telem_lbl.configure(text=raw[:120])

        # Gauge stale detection
        if not d:
            self._draw_gauge(0, stale=True)

        # Status bar
        conn_state = "CONNECTED" if self._connected else "DISCONNECTED"
        cmd_cnt = self.client.cmd_count
        telem_cnt = self.client.telem_count
        csv_state = "LOGGING" if self.client.csv_logging else ""
        self.status_lbl.configure(
            text=f"{conn_state} | Cmd:{cmd_cnt} Tel:{telem_cnt} {csv_state}"
        )

        # Check serial error
        if self.client.error_msg:
            self._console_log(f"[ERR] {self.client.error_msg}")
            self.client.error_msg = ""
            if not self.client.connected and self._connected:
                self._connected = False
                self.conn_status.configure(
                    text=" LOST ", bg=C["red"], fg=C["bg"])
                self.connect_btn.configure(state="normal")
                self.disconnect_btn.configure(state="disabled")

        self.after(TICK_MS, self._tick)

    # ═══════════════════════════════════════════════════════════════
    #  Close
    # ═══════════════════════════════════════════════════════════════

    def _on_close(self):
        self._stop_all_motion()
        if self._connected:
            self.client.safe_stop()
            time.sleep(0.05)
            self.client.disconnect()
        self.destroy()


def main():
    parser = argparse.ArgumentParser(
        description="FTDI H7 GUI — Motor Controller Interface")
    parser.add_argument("--port", default=DEFAULT_PORT,
                        help=f"Serial port (default: {DEFAULT_PORT})")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                        help=f"Baud rate (default: {DEFAULT_BAUD})")
    args = parser.parse_args()

    gui = FtdiH7Gui(args.port, args.baud)
    gui.mainloop()


if __name__ == "__main__":
    main()
