import threading
import queue
import time
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


# H7'den gelen telemetri satirlari bu motor prefix'leri ile gelir
MOTOR_PREFIXES = ("FL", "FR", "RL", "RR")


class SerialManager:
    def __init__(self):
        self.ser = None
        self.read_thread = None
        self.read_running = False
        self.rx_queue = queue.Queue()
        self.status_queue = queue.Queue()
        self.lock = threading.Lock()

    def list_ports(self):
        if list_ports is None:
            return []
        return [p.device for p in list_ports.comports()]

    def is_connected(self):
        return self.ser is not None and self.ser.is_open

    def connect(self, port: str, baudrate: int, timeout: float = 0.1):
        if serial is None:
            raise RuntimeError("pyserial is not installed. Install it with: pip install pyserial")

        self.disconnect()

        self.ser = serial.Serial(port=port, baudrate=baudrate, timeout=timeout)
        self.read_running = True
        self.read_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.read_thread.start()
        self.status_queue.put(("info", f"Connected to {port} @ {baudrate}"))

    def disconnect(self):
        self.read_running = False
        if self.read_thread and self.read_thread.is_alive():
            self.read_thread.join(timeout=0.5)
        self.read_thread = None

        if self.ser:
            try:
                if self.ser.is_open:
                    self.ser.close()
            except Exception:
                pass
        self.ser = None

    def send_line(self, text: str):
        payload = (text.strip() + "\n").encode("utf-8")
        with self.lock:
            if not self.ser or not self.ser.is_open:
                raise RuntimeError("Serial port is not connected")
            self.ser.write(payload)
            self.ser.flush()
        self.status_queue.put(("tx", text.strip()))

    def _reader_loop(self):
        buffer = b""
        max_buffer = 4096
        while self.read_running and self.ser and self.ser.is_open:
            try:
                chunk = self.ser.read(256)
                if not chunk:
                    continue

                buffer += chunk
                if len(buffer) > max_buffer:
                    buffer = buffer[-max_buffer:]

                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    line = line.rstrip(b"\r")
                    try:
                        text = line.decode("utf-8", errors="replace")
                    except Exception:
                        text = repr(line)
                    self.rx_queue.put(text)
            except Exception as exc:
                self.status_queue.put(("error", f"Serial read error: {exc}"))
                self.read_running = False
                break

        self.status_queue.put(("info", "Reader thread stopped"))


class MotorControlGUI(tk.Tk):
    HEARTBEAT_INTERVAL_S = 0.5
    RELEASE_DEBOUNCE_MS = 40

    def __init__(self, debug_heartbeat=False):
        super().__init__()
        self.title("H7 Motor Control GUI  —  PWM / RPM")
        self.geometry("1320x920")
        self.minsize(1180, 820)

        self.serial_mgr = SerialManager()
        self.debug_heartbeat = debug_heartbeat

        # Connection
        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.connection_state_var = tk.StringVar(value="Disconnected")

        # Driving mode: "pwm" or "rpm"
        self.drive_mode_var = tk.StringVar(value="pwm")

        # PWM controls
        self.pwm_var = tk.IntVar(value=100)
        self.pwm_step_var = tk.IntVar(value=5)

        # RPM controls
        self.rpm_var = tk.IntVar(value=23)
        self.rpm_step_var = tk.IntVar(value=5)

        # Speed PI tuning vars
        self.kp_var = tk.StringVar(value="0.8")
        self.ki_var = tk.StringVar(value="0.1")
        self.base_lo_var = tk.StringVar(value="55")
        self.base_mid_var = tk.StringVar(value="45")
        self.base_hi_var = tk.StringVar(value="35")
        self.boost_lo_var = tk.StringVar(value="65")
        self.boost_mid_var = tk.StringVar(value="65")
        self.boost_hi_var = tk.StringVar(value="65")
        self.boost_ms_var = tk.StringVar(value="150")
        self.ramp_up_var = tk.StringVar(value="100")
        self.ramp_down_var = tk.StringVar(value="200")

        self.custom_cmd_var = tk.StringVar()
        self.auto_send_pwm_var = tk.BooleanVar(value=True)  # UI compatibility

        # WheelBridge status tracking
        self.wheelbridge_state_var = tk.StringVar(value="UNKNOWN")

        self.active_keys = set()       # one-shot keys: space, h, i
        self.release_jobs = {}         # debounce release callbacks

        self.key_states = {"w": False, "a": False, "s": False, "d": False}
        self.press_order = {"w": 0, "a": 0, "s": 0, "d": 0}
        self.order_counter = 0

        self.movement_lock = threading.Lock()
        self.movement_state = "inactive"      # inactive / forward / backward / left / right
        self.active_movement_command = None

        self.heartbeat_running = True
        self.heartbeat_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
        self.heartbeat_thread.start()

        # Telemetry storage per motor
        self.telemetry = {
            "FL": self._empty_telem(),
            "FR": self._empty_telem(),
            "RL": self._empty_telem(),
            "RR": self._empty_telem(),
        }

        self._build_ui()
        self._bind_shortcuts()
        self._refresh_ports()
        self.after(100, self._poll_queues)

        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------
    def _empty_telem(self):
        return {
            "rpm": "--", "target": "--", "duty": "--", "dir": "--",
            "phase": "--", "sp": "--", "brake": "--", "fault": "--", "hall": "--",
            "pwm_set": "--", "pwm_act": "--",
        }

    def _build_ui(self):
        self.columnconfigure(0, weight=1)
        self.rowconfigure(3, weight=1)

        # --- Top bar ---
        top = ttk.Frame(self, padding=12)
        top.grid(row=0, column=0, sticky="ew")
        top.columnconfigure(8, weight=1)

        ttk.Label(top, text="Port:").grid(row=0, column=0, sticky="w", padx=(0, 6))
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=18, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky="w")
        ttk.Button(top, text="Refresh", command=self._refresh_ports).grid(row=0, column=2, padx=6)
        ttk.Label(top, text="Baud:").grid(row=0, column=3, sticky="w", padx=(12, 6))
        ttk.Entry(top, textvariable=self.baud_var, width=10).grid(row=0, column=4, sticky="w")
        ttk.Button(top, text="Connect", command=self._connect).grid(row=0, column=5, padx=(12, 6))
        ttk.Button(top, text="Disconnect", command=self._disconnect).grid(row=0, column=6)
        ttk.Button(top, text="EMERGENCY STOP", command=self._emergency_stop).grid(row=0, column=7, padx=(12, 0))
        ttk.Label(top, textvariable=self.connection_state_var).grid(row=0, column=8, sticky="e")

        # --- Control row: 4 panels ---
        control = ttk.Frame(self, padding=(12, 6))
        control.grid(row=1, column=0, sticky="ew")
        for i in range(4):
            control.columnconfigure(i, weight=1)

        self._build_movement_panel(control, 0)
        self._build_rpm_panel(control, 1)
        self._build_speedpi_panel(control, 2)
        self._build_service_panel(control, 3)

        # --- Telemetry table ---
        telemetry_frame = ttk.LabelFrame(self, text="Wheel Telemetry (F411 -> H7 -> PC)", padding=12)
        telemetry_frame.grid(row=2, column=0, sticky="ew", padx=12, pady=(0, 6))
        telemetry_frame.columnconfigure(0, weight=1)

        columns = ("motor", "rpm", "target", "duty", "dir", "phase", "sp", "brake", "fault", "hall", "pwm_set", "pwm_act")
        self.telemetry_table = ttk.Treeview(telemetry_frame, columns=columns, show="headings", height=4)
        headings = {
            "motor": "Motor", "rpm": "RPM", "target": "Target", "duty": "Duty",
            "dir": "Dir", "phase": "Phase", "sp": "Speed", "brake": "Brake",
            "fault": "Fault", "hall": "Hall", "pwm_set": "PW_Set", "pwm_act": "PW_Act",
        }
        for c in columns:
            self.telemetry_table.heading(c, text=headings[c])
            self.telemetry_table.column(c, width=90, anchor="center")
        self.telemetry_table.column("motor", width=60, anchor="center")
        self.telemetry_table.column("pwm_set", width=70, anchor="center")
        self.telemetry_table.column("pwm_act", width=70, anchor="center")
        self.telemetry_table.grid(row=0, column=0, sticky="ew")

        for name in ("FL", "FR", "RL", "RR"):
            self.telemetry_table.insert("", "end", iid=f"motor_{name}",
                                         values=(name, "--", "--", "--", "--", "--", "--", "--", "--", "--", "--", "--"))

        ttk.Label(telemetry_frame,
                  text="Telemetri: wheelbridge on + speed modda surus. Kompakt: RPM,T,D,DIR,PH,SP,BRAKE,FC,H,PWM_SET,PWM_ACT. Debug: TCMD/BASE/OUT ekstra."
                  ).grid(row=1, column=0, sticky="w", pady=(8, 0))

        # --- Console ---
        console_frame = ttk.LabelFrame(self, text="Console", padding=12)
        console_frame.grid(row=3, column=0, sticky="nsew", padx=12, pady=(0, 6))
        console_frame.columnconfigure(0, weight=1)
        console_frame.rowconfigure(0, weight=1)

        self.console = tk.Text(console_frame, wrap="word", state="disabled", font=("Consolas", 10), height=10)
        self.console.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(console_frame, orient="vertical", command=self.console.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.console.configure(yscrollcommand=scrollbar.set)

        # --- Bottom: custom command + clear ---
        bottom = ttk.Frame(self, padding=(12, 0, 12, 12))
        bottom.grid(row=4, column=0, sticky="ew")
        bottom.columnconfigure(0, weight=1)
        ttk.Label(bottom, text="Custom / Raw:").grid(row=0, column=0, sticky="w")
        ttk.Entry(bottom, textvariable=self.custom_cmd_var).grid(row=0, column=1, sticky="ew", padx=(8, 6))
        ttk.Button(bottom, text="Send", command=self._send_custom).grid(row=0, column=2, sticky="ew")
        ttk.Button(bottom, text="Clear Console", command=self._clear_console).grid(row=0, column=3, padx=(8, 0))

    def _build_movement_panel(self, parent, col):
        movement = ttk.LabelFrame(parent, text="Movement (PWM / RPM)", padding=10)
        movement.grid(row=0, column=col, sticky="nsew", padx=(0, 6))
        for i in range(6):
            movement.columnconfigure(i, weight=1)

        # Driving mode selector
        mode_frame = ttk.Frame(movement)
        mode_frame.grid(row=0, column=0, columnspan=6, sticky="ew", pady=(0, 6))
        ttk.Label(mode_frame, text="Mode:").pack(side="left")
        ttk.Radiobutton(mode_frame, text="PWM/Duty", value="pwm",
                        variable=self.drive_mode_var,
                        command=self._on_drive_mode_change).pack(side="left", padx=(6, 0))
        ttk.Radiobutton(mode_frame, text="RPM/Speed", value="rpm",
                        variable=self.drive_mode_var,
                        command=self._on_drive_mode_change).pack(side="left", padx=(6, 0))

        # PWM controls
        self.pwm_row = ttk.Frame(movement)
        self.pwm_row.grid(row=1, column=0, columnspan=6, sticky="ew")
        ttk.Label(self.pwm_row, text="PWM:").grid(row=0, column=0, sticky="w")
        ttk.Spinbox(self.pwm_row, from_=0, to=4000, increment=1, textvariable=self.pwm_var, width=6).grid(row=0, column=1, padx=4)
        ttk.Button(self.pwm_row, text="-", width=3, command=lambda: self._adjust_value(-self._get_step())).grid(row=0, column=2, padx=2)
        ttk.Button(self.pwm_row, text="+", width=3, command=lambda: self._adjust_value(self._get_step())).grid(row=0, column=3, padx=2)
        ttk.Label(self.pwm_row, text="Step:").grid(row=0, column=4, padx=(8, 2))
        ttk.Spinbox(self.pwm_row, from_=1, to=50, increment=1, textvariable=self.pwm_step_var, width=5).grid(row=0, column=5)

        # RPM controls
        self.rpm_row = ttk.Frame(movement)
        self.rpm_row.grid(row=2, column=0, columnspan=6, sticky="ew", pady=(4, 0))
        ttk.Label(self.rpm_row, text="RPM:").grid(row=0, column=0, sticky="w")
        ttk.Spinbox(self.rpm_row, from_=0, to=400, increment=1, textvariable=self.rpm_var, width=6).grid(row=0, column=1, padx=4)
        ttk.Button(self.rpm_row, text="-", width=3, command=lambda: self._adjust_value(-self._get_step())).grid(row=0, column=2, padx=2)
        ttk.Button(self.rpm_row, text="+", width=3, command=lambda: self._adjust_value(self._get_step())).grid(row=0, column=3, padx=2)
        ttk.Label(self.rpm_row, text="Step:").grid(row=0, column=4, padx=(8, 2))
        ttk.Spinbox(self.rpm_row, from_=1, to=50, increment=1, textvariable=self.rpm_step_var, width=5).grid(row=0, column=5)

        ttk.Label(movement,
                  text="Hotkeys: W/A/S/D move, +/- value, Space stop, H help, I status"
                  ).grid(row=3, column=0, columnspan=6, sticky="w", pady=(6, 4))

        ttk.Button(movement, text="Forward (W)", command=lambda: self._send_motion("forward")).grid(row=4, column=1, pady=4, padx=4, sticky="ew")
        ttk.Button(movement, text="Left (A)", command=lambda: self._send_motion("left")).grid(row=5, column=0, pady=4, padx=4, sticky="ew")
        ttk.Button(movement, text="Stop (Space)", command=self._force_stop).grid(row=5, column=1, pady=4, padx=4, sticky="ew")
        ttk.Button(movement, text="Right (D)", command=lambda: self._send_motion("right")).grid(row=5, column=2, pady=4, padx=4, sticky="ew")
        ttk.Button(movement, text="Backward (S)", command=lambda: self._send_motion("backward")).grid(row=6, column=1, pady=4, padx=4, sticky="ew")

        self._on_drive_mode_change()

    def _build_rpm_panel(self, parent, col):
        panel = ttk.LabelFrame(parent, text="RPM & Speed Mode", padding=10)
        panel.grid(row=0, column=col, sticky="nsew", padx=6)
        for i in range(4):
            panel.columnconfigure(i, weight=1)

        ttk.Label(panel, text="Quick RPM:").grid(row=0, column=0, columnspan=4, sticky="w")
        r = 1
        for val in (10, 15, 23, 50, 100, 300, 400):
            ttk.Button(panel, text=str(val), width=5,
                       command=lambda v=val: self._set_quick_rpm(v)).grid(row=r // 4 + 1, column=r % 4, pady=2, padx=2, sticky="ew")
            r += 1
        ttk.Button(panel, text="RPM Stop", command=self._send_rpm_stop).grid(row=3, column=0, columnspan=4, pady=(6, 0), sticky="ew")

        ttk.Separator(panel, orient="horizontal").grid(row=4, column=0, columnspan=4, sticky="ew", pady=8)

        ttk.Button(panel, text="Enable Speed Mode",
                   command=lambda: self._send_simple("speed all on")).grid(row=5, column=0, columnspan=4, sticky="ew", pady=2)
        ttk.Button(panel, text="Disable Speed Mode (Duty)",
                   command=lambda: self._send_simple("speed all off")).grid(row=6, column=0, columnspan=4, sticky="ew", pady=2)
        ttk.Button(panel, text="WheelBridge On",
                   command=lambda: self._set_wheelbridge(True)).grid(row=7, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        ttk.Button(panel, text="WheelBridge Off",
                   command=lambda: self._set_wheelbridge(False)).grid(row=7, column=2, columnspan=2, sticky="ew", pady=(6, 0))
        ttk.Label(panel, textvariable=self.wheelbridge_state_var,
                  foreground="gray").grid(row=8, column=0, columnspan=4, sticky="w", pady=(2, 0))

    def _build_speedpi_panel(self, parent, col):
        panel = ttk.LabelFrame(parent, text="Speed PI Tuning", padding=10)
        panel.grid(row=0, column=col, sticky="nsew", padx=6)
        panel.columnconfigure(1, weight=1)
        panel.columnconfigure(3, weight=1)
        panel.columnconfigure(5, weight=1)

        rows = [
            ("Kp", self.kp_var, "Ki", self.ki_var, "", None),
            ("base lo", self.base_lo_var, "mid", self.base_mid_var, "hi", self.base_hi_var),
            ("boost lo", self.boost_lo_var, "mid", self.boost_mid_var, "hi", self.boost_hi_var),
            ("boost ms", self.boost_ms_var, "ramp up", self.ramp_up_var, "down", self.ramp_down_var),
        ]
        for r, (l1, v1, l2, v2, l3, v3) in enumerate(rows):
            ttk.Label(panel, text=l1).grid(row=r, column=0, sticky="w", padx=(0, 4))
            ttk.Entry(panel, textvariable=v1, width=6).grid(row=r, column=1, sticky="ew", padx=(0, 8))
            ttk.Label(panel, text=l2).grid(row=r, column=2, sticky="w", padx=(0, 4))
            ttk.Entry(panel, textvariable=v2, width=6).grid(row=r, column=3, sticky="ew", padx=(0, 8))
            if v3 is not None:
                ttk.Label(panel, text=l3).grid(row=r, column=4, sticky="w", padx=(0, 4))
                ttk.Entry(panel, textvariable=v3, width=6).grid(row=r, column=5, sticky="ew")

        ttk.Button(panel, text="Apply Speed PI Settings",
                   command=self._apply_speed_pi).grid(row=len(rows), column=0, columnspan=6, sticky="ew", pady=(8, 2))
        ttk.Button(panel, text="spstat all",
                   command=lambda: self._send_simple("spstat all")).grid(row=len(rows) + 1, column=0, columnspan=6, sticky="ew", pady=2)

    def _build_service_panel(self, parent, col):
        panel = ttk.LabelFrame(parent, text="Service Tools", padding=10)
        panel.grid(row=0, column=col, sticky="nsew", padx=(6, 0))
        for i in range(4):
            panel.columnconfigure(i, weight=1)

        buttons = [
            ("Hall FL", "hall fl"), ("Hall FR", "hall fr"), ("Hall All", "hall all"),
            ("Status All", "status all"), ("Spstat All", "spstat all"), ("Identify All", "identify all"),
            ("Identify FL", "identify fl"), ("Identify FR", "identify fr"), ("Map All", "map all"),
            ("Save All", "save all"), ("Reload All", "reload all"), ("MapReset All", "mapreset all"),
            ("Clear Fault All", "clrerr all"), ("H7 Status", "status"), ("Help", "help"),
        ]
        for i, (label, cmd) in enumerate(buttons):
            ttk.Button(panel, text=label,
                       command=lambda c=cmd: self._send_simple(c)).grid(row=i // 3, column=i % 3, pady=2, padx=2, sticky="ew")

    # ------------------------------------------------------------------
    # Shortcuts / ports / connection
    # ------------------------------------------------------------------
    def _bind_shortcuts(self):
        self.bind_all("<KeyPress>", self._handle_keypress)
        self.bind_all("<KeyRelease>", self._handle_keyrelease)

    def _refresh_ports(self):
        ports = self.serial_mgr.list_ports()
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _connect(self):
        port = self.port_var.get().strip()
        if not port:
            messagebox.showerror("Port Error", "Select a serial port first.")
            return

        try:
            baud = int(self.baud_var.get().strip())
        except ValueError:
            messagebox.showerror("Baud Error", "Baud rate must be an integer.")
            return

        try:
            self.serial_mgr.connect(port, baud)
            self.connection_state_var.set(f"Connected: {port} @ {baud}")
        except Exception as exc:
            messagebox.showerror("Connection Error", str(exc))
            self.connection_state_var.set("Disconnected")

    def _disconnect(self):
        # Baglanti koparken motorlari guvenle durdur
        self._safe_stop_silent()
        self.serial_mgr.disconnect()
        self._clear_movement_state(send_stop=False)
        self.connection_state_var.set("Disconnected")
        self._log("[INFO] Disconnected")

    def _on_close(self):
        self.heartbeat_running = False
        try:
            self._safe_stop_silent()
        except Exception:
            pass
        try:
            self.serial_mgr.disconnect()
        except Exception:
            pass
        self.destroy()

    # ------------------------------------------------------------------
    # Value helpers
    # ------------------------------------------------------------------
    def _get_pwm(self) -> int:
        try:
            pwm = int(self.pwm_var.get())
        except Exception:
            pwm = 0
        pwm = max(0, min(4000, pwm))
        self.pwm_var.set(pwm)
        return pwm

    def _get_rpm(self) -> int:
        try:
            rpm = int(self.rpm_var.get())
        except Exception:
            rpm = 0
        rpm = max(0, min(400, rpm))
        self.rpm_var.set(rpm)
        return rpm

    def _get_step(self) -> int:
        if self.drive_mode_var.get() == "rpm":
            try:
                step = int(self.rpm_step_var.get())
            except Exception:
                step = 1
            return max(1, min(50, step))
        try:
            step = int(self.pwm_step_var.get())
        except Exception:
            step = 1
        return max(1, min(50, step))

    def _adjust_value(self, delta: int):
        if self.drive_mode_var.get() == "rpm":
            new_v = max(0, min(400, self._get_rpm() + delta))
            self.rpm_var.set(new_v)
        else:
            new_v = max(0, min(4000, self._get_pwm() + delta))
            self.pwm_var.set(new_v)
        self._refresh_active_movement_command()

    def _set_quick_rpm(self, val: int):
        self.rpm_var.set(max(0, min(400, val)))
        self._refresh_active_movement_command()

    def _on_drive_mode_change(self):
        mode = self.drive_mode_var.get()
        if mode == "rpm":
            self.pwm_row.grid_remove()
            self.rpm_row.grid()
        else:
            self.rpm_row.grid_remove()
            self.pwm_row.grid()
        self._refresh_active_movement_command()

    # ------------------------------------------------------------------
    # Movement command building
    # ------------------------------------------------------------------
    def _build_movement_command(self, motion_name: str) -> str:
        if self.drive_mode_var.get() == "rpm":
            rpm = self._get_rpm()
            if motion_name == "backward":
                rpm = -rpm
            elif motion_name in ("left", "right"):
                rpm = -rpm if motion_name == "left" else rpm
            return f"rpm {rpm}"
        return f"{motion_name} {self._get_pwm()}"

    def _refresh_active_movement_command(self):
        with self.movement_lock:
            if self.movement_state in {"forward", "backward", "left", "right"}:
                self.active_movement_command = self._build_movement_command(self.movement_state)

    def _set_movement_state(self, state: str, command):
        with self.movement_lock:
            self.movement_state = state
            self.active_movement_command = command

    def _get_movement_snapshot(self):
        with self.movement_lock:
            return self.movement_state, self.active_movement_command

    def _clear_movement_state(self, send_stop: bool = True):
        for key in list(self.release_jobs.keys()):
            self._cancel_release_job(key)

        for key in self.key_states:
            self.key_states[key] = False
            self.press_order[key] = 0

        self.active_keys.clear()
        self.order_counter = 0
        self._set_movement_state("inactive", None)

        if send_stop:
            # RPM modunda rpm 0 (F411 speed modda kalir), PWM modunda stop
            if self.drive_mode_var.get() == "rpm":
                self._send_simple("rpm 0")
            else:
                self._send_simple("stop")

    def _force_stop(self):
        self._clear_movement_state(send_stop=True)

    def _emergency_stop(self):
        self._clear_movement_state(send_stop=False)
        # Iki modu da guvenle durdur — rpm 0 once (speed modda kalir), sonra stop
        self._send_simple("rpm 0")
        self._send_simple("stop")
        self._send_simple("raw all rpm 0")
        self._log("[E-STOP] sent rpm 0 + stop + raw all rpm 0")

    def _send_rpm_stop(self):
        self._send_simple("rpm 0")

    def _safe_stop_silent(self):
        # Baglanti varken sessiz guvenli stop (kapatma/baglanti kesme icin)
        # rpm 0 once — F411 speed modda kalir ve durur; sonra stop duty modda durur
        if self.serial_mgr.is_connected():
            try:
                self.serial_mgr.send_line("rpm 0")
            except Exception:
                pass
            try:
                self.serial_mgr.send_line("raw all rpm 0")
            except Exception:
                pass
            try:
                self.serial_mgr.send_line("stop")
            except Exception:
                pass

    def _cancel_release_job(self, key: str):
        job = self.release_jobs.pop(key, None)
        if job is not None:
            try:
                self.after_cancel(job)
            except Exception:
                pass

    # ------------------------------------------------------------------
    # Heartbeat
    # ------------------------------------------------------------------
    def _heartbeat_loop(self):
        while self.heartbeat_running:
            time.sleep(self.HEARTBEAT_INTERVAL_S)
            state, cmd = self._get_movement_snapshot()

            if state in {"forward", "backward", "left", "right"} and cmd and self.serial_mgr.is_connected():
                try:
                    self.serial_mgr.send_line(cmd)
                    if self.debug_heartbeat:
                        self.serial_mgr.status_queue.put(("info", f"[HB] sent: {cmd}"))
                except Exception as exc:
                    if self.debug_heartbeat:
                        self.serial_mgr.status_queue.put(("info", f"[HB] FAIL: {cmd} | {exc}"))
                    self.serial_mgr.status_queue.put(("error", f"Heartbeat send error: {exc}"))

    def _highest_priority_pressed_key(self):
        pressed = [k for k, v in self.key_states.items() if v]
        if not pressed:
            return None
        return max(pressed, key=lambda k: self.press_order.get(k, 0))

    def _recompute_movement_state(self):
        chosen_key = self._highest_priority_pressed_key()
        current_state, current_cmd = self._get_movement_snapshot()

        if chosen_key is None:
            if current_state != "inactive":
                self._set_movement_state("inactive", None)
                # RPM modunda rpm 0 (F411 speed modda kalir), PWM modunda stop
                if self.drive_mode_var.get() == "rpm":
                    self._send_simple("rpm 0")
                else:
                    self._send_simple("stop")
            return

        key_to_motion = {"w": "forward", "s": "backward", "a": "left", "d": "right"}
        desired_state = key_to_motion[chosen_key]
        desired_cmd = self._build_movement_command(desired_state)

        if current_state != desired_state or current_cmd != desired_cmd:
            self._set_movement_state(desired_state, desired_cmd)
            self._send_line(desired_cmd)

    # ------------------------------------------------------------------
    # Key handling
    # ------------------------------------------------------------------
    def _handle_keypress(self, event):
        focus_widget = self.focus_get()
        if isinstance(focus_widget, (tk.Entry, ttk.Entry, ttk.Spinbox)):
            return

        key = event.keysym.lower()

        if key in {"w", "a", "s", "d"}:
            self._cancel_release_job(key)
            if not self.key_states[key]:
                self.key_states[key] = True
                self.order_counter += 1
                self.press_order[key] = self.order_counter
                self._recompute_movement_state()
            return

        if key in {"plus", "equal", "kp_add"}:
            self._adjust_value(self._get_step())
            return

        if key in {"minus", "underscore", "kp_subtract"}:
            self._adjust_value(-self._get_step())
            return

        if key in {"space", "h", "i"}:
            if key in self.active_keys:
                return
            self.active_keys.add(key)
            if key == "space":
                self._force_stop()
            elif key == "h":
                self._send_simple("help")
            elif key == "i":
                self._send_simple("status")

    def _handle_keyrelease(self, event):
        focus_widget = self.focus_get()
        if isinstance(focus_widget, (tk.Entry, ttk.Entry, ttk.Spinbox)):
            return

        key = event.keysym.lower()

        if key in self.active_keys:
            self.active_keys.discard(key)

        if key in {"w", "a", "s", "d"}:
            self._cancel_release_job(key)
            self.release_jobs[key] = self.after(
                self.RELEASE_DEBOUNCE_MS,
                lambda k=key: self._finalize_movement_release(k)
            )

    def _finalize_movement_release(self, key: str):
        self.release_jobs.pop(key, None)
        if key not in self.key_states:
            return
        self.key_states[key] = False
        self.press_order[key] = 0
        self._recompute_movement_state()

    # ------------------------------------------------------------------
    # Send helpers
    # ------------------------------------------------------------------
    def _send_motion(self, cmd: str):
        motion_cmd = self._build_movement_command(cmd)
        self._set_movement_state(cmd, motion_cmd)
        self._send_line(motion_cmd)

    def _send_simple(self, cmd: str):
        self._send_line(cmd)

    def _set_wheelbridge(self, enabled: bool):
        cmd = "wheelbridge on" if enabled else "wheelbridge off"
        self._send_simple(cmd)
        self.wheelbridge_state_var.set(f"WheelBridge: {'ON' if enabled else 'OFF'}")

    def _send_custom(self):
        text = self.custom_cmd_var.get().strip()
        if not text:
            return
        self._send_line(text)
        self.custom_cmd_var.set("")

    def _send_line(self, text: str):
        try:
            self.serial_mgr.send_line(text)
        except Exception as exc:
            messagebox.showerror("Send Error", str(exc))

    def _apply_speed_pi(self):
        # H7 uzerinden tum F411'lere speed PI ayarlarini gonder
        kp = self.kp_var.get().strip()
        ki = self.ki_var.get().strip()
        blo = self.base_lo_var.get().strip()
        bmid = self.base_mid_var.get().strip()
        bhi = self.base_hi_var.get().strip()
        lo = self.boost_lo_var.get().strip()
        mid = self.boost_mid_var.get().strip()
        hi = self.boost_hi_var.get().strip()
        ms = self.boost_ms_var.get().strip()
        rup = self.ramp_up_var.get().strip()
        rdn = self.ramp_down_var.get().strip()

        self._send_simple(f"pi all {kp} {ki}")
        self._send_simple(f"base all {blo} {bmid} {bhi}")
        self._send_simple(f"boost all {lo} {mid} {hi} {ms}")
        self._send_simple(f"ramp all {rup} {rdn}")

    # ------------------------------------------------------------------
    # Queue polling + telemetry parsing
    # ------------------------------------------------------------------
    def _poll_queues(self):
        while not self.serial_mgr.rx_queue.empty():
            line = self.serial_mgr.rx_queue.get_nowait()
            if not self._parse_telemetry(line):
                self._log(f"[RX] {line}")

        while not self.serial_mgr.status_queue.empty():
            level, msg = self.serial_mgr.status_queue.get_nowait()
            if level == "tx":
                self._log(f"[TX] {msg}")
            elif level == "error":
                self._log(f"[ERROR] {msg}")
            else:
                self._log(f"[INFO] {msg}")

        self.after(100, self._poll_queues)

    def _parse_telemetry(self, line: str) -> bool:
        # H7 telemetri kopru formati: <MOTOR>|<key:val,key:val,...>
        if "|" not in line:
            return False
        prefix, _, payload = line.partition("|")
        prefix = prefix.strip().upper()
        if prefix not in MOTOR_PREFIXES:
            return False
        # Telemetri indicator: RPM: anahtari var
        if "RPM:" not in payload:
            return False

        fields = {"rpm": "--", "target": "--", "duty": "--", "dir": "--",
                  "phase": "--", "sp": "--", "brake": "--", "fault": "--", "hall": "--",
                  "pwm_set": "--", "pwm_act": "--"}
        # F411 kompakt: RPM,T,D,DIR,APP_PH,SP,BRAKE,FC,H,PWM_SET,PWM_ACT
        # F411 debug:   RPM,RF,TCMD,TRMP,ERR,BASE,OUT,I,D,APP_PH,FC,PWM_SET,PWM_ACT
        # Gelen key'ler .upper() ile buyuk harfe cevrilir — key_map de buyuk olmali
        key_map = {
            "RPM": "rpm", "T": "target", "TCMD": "target", "D": "duty",
            "DIR": "dir", "APP_PH": "phase", "PH": "phase", "SP": "sp", "BRAKE": "brake",
            "FC": "fault", "H": "hall",
            "PWM_SET": "pwm_set", "PWM_ACT": "pwm_act",
        }
        for part in payload.split(","):
            if ":" not in part:
                continue
            k, _, v = part.partition(":")
            k = k.strip().upper()
            v = v.strip()
            if k in key_map:
                fields[key_map[k]] = v

        self.telemetry[prefix] = fields
        self.telemetry_table.item(f"motor_{prefix}",
                                  values=(prefix, fields["rpm"], fields["target"], fields["duty"],
                                          fields["dir"], fields["phase"], fields["sp"], fields["brake"],
                                          fields["fault"], fields["hall"], fields["pwm_set"], fields["pwm_act"]))
        return True

    # ------------------------------------------------------------------
    # Console
    # ------------------------------------------------------------------
    def _log(self, text: str):
        timestamp = time.strftime("%H:%M:%S")
        self.console.configure(state="normal")
        self.console.insert("end", f"[{timestamp}] {text}\n")
        line_count = int(self.console.index("end-1c").split(".")[0])
        if line_count > 2000:
            self.console.delete("1.0", f"{line_count - 2000}.0")
        self.console.see("end")
        self.console.configure(state="disabled")

    def _clear_console(self):
        self.console.configure(state="normal")
        self.console.delete("1.0", "end")
        self.console.configure(state="disabled")


if __name__ == "__main__":
    import sys
    debug_hb = "--debug-heartbeat" in sys.argv
    app = MotorControlGUI(debug_heartbeat=debug_hb)
    app.mainloop()
