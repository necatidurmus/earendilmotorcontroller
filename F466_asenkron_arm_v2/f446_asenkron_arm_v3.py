import threading
import queue
import time
import socket
from typing import Optional
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


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
            raise RuntimeError("pyserial is not installed. Install with: pip install pyserial")
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
        if not self.is_connected():
            raise RuntimeError("Serial port is not connected")
        clean = text.strip()
        payload = (clean + "\n").encode("utf-8")
        with self.lock:
            self.ser.write(payload)
            self.ser.flush()
        # Avoid console spam for high-rate arm joystick stream.
        if not clean.lower().startswith("arm j,"):
            self.status_queue.put(("tx", clean))

    def _reader_loop(self):
        buffer = b""
        while self.read_running and self.ser and self.ser.is_open:
            try:
                chunk = self.ser.read(256)
                if not chunk:
                    continue
                buffer += chunk
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

    UDP_PORT = 5005
    UDP_FAILSAFE_TIMEOUT_S = 2.0
    UDP_WATCHDOG_INTERVAL_S = 0.1
    UDP_DEBUG_LOG_INTERVAL_S = 0.5

    def __init__(self):
        super().__init__()
        self.title("F446RE Rover Control - FL/FR + Arm Bridge")
        self.geometry("1120x900")
        self.minsize(980, 760)

        self.serial_mgr = SerialManager()

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.pwm_var = tk.IntVar(value=100)
        self.pwm_step_var = tk.IntVar(value=5)
        self.custom_cmd_var = tk.StringVar()
        self.connection_state_var = tk.StringVar(value="Disconnected")
        self.udp_state_var = tk.StringVar(value=f"UDP arm listener: 0.0.0.0:{self.UDP_PORT}")

        self.active_keys = set()
        self.release_jobs = {}
        self.key_states = {"w": False, "a": False, "s": False, "d": False}
        self.press_order = {"w": 0, "a": 0, "s": 0, "d": 0}
        self.order_counter = 0

        self.movement_lock = threading.Lock()
        self.movement_state = "inactive"
        self.active_movement_command = None

        self.heartbeat_running = True
        self.heartbeat_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
        self.heartbeat_thread.start()

        self.udp_running = True
        self.udp_lock = threading.Lock()
        self.udp_gamepad_active = False
        self.udp_last_gamepad_time = 0.0
        self.udp_failsafe_sent = True
        self.udp_last_debug_log_time = 0.0
        self.udp_thread = threading.Thread(target=self._udp_listener_loop, daemon=True)
        self.udp_thread.start()
        self.udp_watchdog_thread = threading.Thread(target=self._udp_failsafe_loop, daemon=True)
        self.udp_watchdog_thread.start()

        self.motor_placeholder = {
            "FL": {"pwm": tk.StringVar(value="--"), "voltage": tk.StringVar(value="-- V"), "current": tk.StringVar(value="-- A")},
            "FR": {"pwm": tk.StringVar(value="--"), "voltage": tk.StringVar(value="-- V"), "current": tk.StringVar(value="-- A")},
        }

        self.arm_data = {
            "M1": {"pwm": "0", "angle": "0.0"},
            "M2": {"pwm": "0", "angle": "0.0"},
            "M3": {"pwm": "0", "angle": "0.0"},
            "M4": {"pwm": "0", "angle": "N/A"},
            "M5": {"pwm": "0", "angle": "N/A"},
            "M6": {"pwm": "0", "angle": "N/A"},
        }
        self.arm_name_alias = {
            "M1": "M1", "M2": "M2", "M3": "M3", "M4": "M4", "M5": "M5", "M6": "M6",
            "J1": "M1", "J2": "M2", "J3": "M3", "TW": "M4", "PI": "M5", "GR": "M6",
        }

        self._build_ui()
        self._bind_shortcuts()
        self._refresh_ports()
        self.after(100, self._poll_queues)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self):
        self.columnconfigure(0, weight=1)
        self.rowconfigure(4, weight=1)

        top = ttk.Frame(self, padding=12)
        top.grid(row=0, column=0, sticky="ew")
        for i in range(10):
            top.columnconfigure(i, weight=0)
        top.columnconfigure(8, weight=1)

        ttk.Label(top, text="Port:").grid(row=0, column=0, sticky="w", padx=(0, 6))
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=18, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky="w")
        ttk.Button(top, text="Refresh", command=self._refresh_ports).grid(row=0, column=2, padx=6)
        ttk.Label(top, text="Baud:").grid(row=0, column=3, sticky="w", padx=(12, 6))
        ttk.Entry(top, textvariable=self.baud_var, width=10).grid(row=0, column=4, sticky="w")
        ttk.Button(top, text="Connect", command=self._connect).grid(row=0, column=5, padx=(12, 6))
        ttk.Button(top, text="Disconnect", command=self._disconnect).grid(row=0, column=6)
        ttk.Label(top, textvariable=self.connection_state_var).grid(row=0, column=8, sticky="e")

        middle = ttk.Frame(self, padding=(12, 0, 12, 12))
        middle.grid(row=1, column=0, sticky="ew")
        middle.columnconfigure(0, weight=1)
        middle.columnconfigure(1, weight=1)
        middle.columnconfigure(2, weight=1)

        movement = ttk.LabelFrame(middle, text="Rover Movement - FL/FR Only", padding=12)
        movement.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        for i in range(6):
            movement.columnconfigure(i, weight=1)
        ttk.Label(movement, text="PWM:").grid(row=0, column=0, sticky="w")
        ttk.Spinbox(movement, from_=0, to=255, increment=1, textvariable=self.pwm_var, width=8).grid(row=0, column=1, sticky="w", padx=(8, 6))
        ttk.Button(movement, text="-", width=3, command=lambda: self._adjust_pwm(-self._get_pwm_step())).grid(row=0, column=2, sticky="ew", padx=4)
        ttk.Button(movement, text="+", width=3, command=lambda: self._adjust_pwm(self._get_pwm_step())).grid(row=0, column=3, sticky="ew", padx=4)
        ttk.Label(movement, text="Step:").grid(row=0, column=4, sticky="e", padx=(12, 6))
        ttk.Spinbox(movement, from_=1, to=50, increment=1, textvariable=self.pwm_step_var, width=6).grid(row=0, column=5, sticky="w")
        ttk.Label(movement, text="Hotkeys: W/A/S/D move, +/- PWM, Space stop all, H help, I status").grid(row=1, column=0, columnspan=6, sticky="w", pady=(8, 4))
        ttk.Button(movement, text="Forward (W)", command=lambda: self._send_motion("forward")).grid(row=2, column=1, pady=8, padx=4, sticky="ew")
        ttk.Button(movement, text="Left (A)", command=lambda: self._send_motion("left")).grid(row=3, column=0, pady=8, padx=4, sticky="ew")
        ttk.Button(movement, text="Stop All (Space)", command=self._force_stop).grid(row=3, column=1, pady=8, padx=4, sticky="ew")
        ttk.Button(movement, text="Right (D)", command=lambda: self._send_motion("right")).grid(row=3, column=2, pady=8, padx=4, sticky="ew")
        ttk.Button(movement, text="Backward (S)", command=lambda: self._send_motion("backward")).grid(row=4, column=1, pady=8, padx=4, sticky="ew")

        special = ttk.LabelFrame(middle, text="Special / Arm Commands", padding=12)
        special.grid(row=0, column=1, sticky="nsew", padx=6)
        for i in range(2):
            special.columnconfigure(i, weight=1)
        ttk.Button(special, text="Help (H)", command=lambda: self._send_simple("help")).grid(row=0, column=0, padx=4, pady=4, sticky="ew")
        ttk.Button(special, text="Status (I)", command=lambda: self._send_simple("status")).grid(row=0, column=1, padx=4, pady=4, sticky="ew")
        ttk.Button(special, text="Identify FL/FR", command=lambda: self._send_simple("identify")).grid(row=1, column=0, padx=4, pady=4, sticky="ew")
        ttk.Button(special, text="Stop All", command=self._force_stop).grid(row=1, column=1, padx=4, pady=4, sticky="ew")
        ttk.Button(special, text="Arm Stop", command=lambda: self._send_simple("arm stop")).grid(row=2, column=0, padx=4, pady=4, sticky="ew")
        ttk.Button(special, text="Arm Bridge ON", command=lambda: self._send_simple("armbridge on")).grid(row=2, column=1, padx=4, pady=4, sticky="ew")
        ttk.Button(special, text="Arm Bridge OFF", command=lambda: self._send_simple("armbridge off")).grid(row=3, column=0, padx=4, pady=4, sticky="ew")
        ttk.Button(special, text="Arm Bridge Status", command=lambda: self._send_simple("armbridge status")).grid(row=3, column=1, padx=4, pady=4, sticky="ew")

        custom = ttk.LabelFrame(middle, text="Custom Command", padding=12)
        custom.grid(row=0, column=2, sticky="nsew", padx=(6, 0))
        custom.columnconfigure(0, weight=1)
        ttk.Entry(custom, textvariable=self.custom_cmd_var).grid(row=0, column=0, sticky="ew", padx=(0, 8))
        ttk.Button(custom, text="Send", command=self._send_custom).grid(row=0, column=1, sticky="ew")
        ttk.Label(custom, text="Arm examples: arm stop | arm J,0,0,0,0,0,0").grid(row=1, column=0, columnspan=2, sticky="w", pady=(8, 0))
        ttk.Label(custom, textvariable=self.udp_state_var).grid(row=2, column=0, columnspan=2, sticky="w", pady=(4, 0))

        telemetry_frame = ttk.LabelFrame(self, text="Rover Wheel Telemetry (FL/FR Placeholder)", padding=12)
        telemetry_frame.grid(row=2, column=0, sticky="ew", padx=12, pady=(0, 12))
        telemetry_frame.columnconfigure(0, weight=1)
        columns = ("motor", "pwm", "voltage", "current")
        self.telemetry_table = ttk.Treeview(telemetry_frame, columns=columns, show="headings", height=2)
        for col, title, width in [("motor", "Motor", 120), ("pwm", "PWM", 120), ("voltage", "Voltage", 160), ("current", "Current", 140)]:
            self.telemetry_table.heading(col, text=title)
            self.telemetry_table.column(col, width=width, anchor="center")
        self.telemetry_table.grid(row=0, column=0, sticky="ew")
        for motor_name in ("FL", "FR"):
            self.telemetry_table.insert("", "end", iid=f"motor_{motor_name}", values=(motor_name, "--", "-- V", "-- A"))

        arm_frame = ttk.LabelFrame(self, text="Mechanical Arm Telemetry (requires armbridge on and TEL | lines)", padding=12)
        arm_frame.grid(row=3, column=0, sticky="ew", padx=12, pady=(0, 12))
        arm_frame.columnconfigure(0, weight=1)
        columns_arm = ("joint", "pwm", "angle")
        self.arm_table = ttk.Treeview(arm_frame, columns=columns_arm, show="headings", height=6)
        for col, title, width in [("joint", "Joint/Motor", 120), ("pwm", "PWM Output", 140), ("angle", "AS5600 Angle (°)", 180)]:
            self.arm_table.heading(col, text=title)
            self.arm_table.column(col, width=width, anchor="center")
        self.arm_table.grid(row=0, column=0, sticky="ew")
        for j in ["M1", "M2", "M3", "M4", "M5", "M6"]:
            self.arm_table.insert("", "end", iid=f"arm_{j}", values=(j, self.arm_data[j]["pwm"], self.arm_data[j]["angle"]))

        console_frame = ttk.LabelFrame(self, text="Console", padding=12)
        console_frame.grid(row=4, column=0, sticky="nsew", padx=12, pady=(0, 12))
        console_frame.columnconfigure(0, weight=1)
        console_frame.rowconfigure(0, weight=1)
        self.console = tk.Text(console_frame, wrap="word", state="disabled", font=("Consolas", 10), height=8)
        self.console.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(console_frame, orient="vertical", command=self.console.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.console.configure(yscrollcommand=scrollbar.set)

        bottom = ttk.Frame(self, padding=(12, 0, 12, 12))
        bottom.grid(row=5, column=0, sticky="ew")
        ttk.Button(bottom, text="Clear Console", command=self._clear_console).pack(side="right")

    def _normalize_udp_arm_command(self, msg: str) -> Optional[str]:
        msg = msg.strip()
        if not msg:
            return None
        lowered = msg.lower()
        if lowered == "arm" or lowered.startswith("arm "):
            payload = msg[3:].strip()
        else:
            payload = msg
        if not payload:
            return None
        return f"arm {payload}"

    def _mark_udp_gamepad_packet(self, payload: str):
        payload_lower = payload.strip().lower()
        now = time.time()
        with self.udp_lock:
            if payload_lower.startswith("j,"):
                self.udp_gamepad_active = True
                self.udp_last_gamepad_time = now
                self.udp_failsafe_sent = False
            elif payload_lower == "stop":
                self.udp_gamepad_active = False
                self.udp_failsafe_sent = True
        self.udp_last_debug_log_time = 0.0

    def _udp_listener_loop(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.bind(("0.0.0.0", self.UDP_PORT))
            sock.settimeout(0.5)
            self.serial_mgr.status_queue.put(("info", f"UDP listener active on 0.0.0.0:{self.UDP_PORT}"))
        except Exception as exc:
            self.serial_mgr.status_queue.put(("error", f"UDP bind/listen error: {exc}"))
            return

        while self.udp_running:
            try:
                data, _addr = sock.recvfrom(1024)
                msg = data.decode("utf-8", errors="replace").strip()
                h7_cmd = self._normalize_udp_arm_command(msg)
                if not h7_cmd:
                    continue
                payload = h7_cmd[4:].strip()
                self._mark_udp_gamepad_packet(payload)

                # Throttled debug line: proves terminal.py is forwarding the exact PWM values
                # received from gamepad_udp.py without scaling or clipping.
                now = time.time()
                if payload.lower().startswith("j,") and (now - self.udp_last_debug_log_time) >= self.UDP_DEBUG_LOG_INTERVAL_S:
                    self.udp_last_debug_log_time = now
                    self.serial_mgr.status_queue.put(("info", f"UDP->SERIAL {h7_cmd}"))

                if self.serial_mgr.is_connected():
                    self.serial_mgr.send_line(h7_cmd)
            except socket.timeout:
                continue
            except Exception as exc:
                self.serial_mgr.status_queue.put(("error", f"UDP receive/forward error: {exc}"))
                time.sleep(0.01)
        try:
            sock.close()
        except Exception:
            pass

    def _udp_failsafe_loop(self):
        while self.udp_running:
            time.sleep(self.UDP_WATCHDOG_INTERVAL_S)
            should_stop = False
            with self.udp_lock:
                if self.udp_gamepad_active and not self.udp_failsafe_sent:
                    elapsed = time.time() - self.udp_last_gamepad_time
                    if elapsed >= self.UDP_FAILSAFE_TIMEOUT_S:
                        self.udp_failsafe_sent = True
                        self.udp_gamepad_active = False
                        should_stop = True
            if should_stop:
                try:
                    if self.serial_mgr.is_connected():
                        self.serial_mgr.send_line("arm stop")
                    self.serial_mgr.status_queue.put(("error", "UDP gamepad failsafe: no packet received, sent arm stop"))
                except Exception as exc:
                    self.serial_mgr.status_queue.put(("error", f"UDP failsafe send error: {exc}"))

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
        self.serial_mgr.disconnect()
        self._clear_movement_state(send_stop=False)
        self.connection_state_var.set("Disconnected")
        self._log("[INFO] Disconnected")

    def _on_close(self):
        self.heartbeat_running = False
        self.udp_running = False
        try:
            self._disconnect()
        except Exception:
            pass
        self.destroy()

    def _get_pwm(self) -> int:
        try:
            pwm = int(self.pwm_var.get())
        except Exception:
            pwm = 0
        pwm = max(0, min(255, pwm))
        self.pwm_var.set(pwm)
        return pwm

    def _get_pwm_step(self) -> int:
        try:
            step = int(self.pwm_step_var.get())
        except Exception:
            step = 1
        step = max(1, min(50, step))
        self.pwm_step_var.set(step)
        return step

    def _build_movement_command(self, motion_name: str) -> str:
        return f"{motion_name} {self._get_pwm()}"

    def _adjust_pwm(self, delta: int):
        new_pwm = max(0, min(255, self._get_pwm() + delta))
        self.pwm_var.set(new_pwm)
        self._refresh_active_movement_command()

    def _refresh_active_movement_command(self):
        with self.movement_lock:
            if self.movement_state in {"forward", "backward", "left", "right"}:
                self.active_movement_command = self._build_movement_command(self.movement_state)

    def _set_movement_state(self, state: str, command: str | None):
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
            self._send_simple("stop")

    def _force_stop(self):
        # Stop All: rover wheels + mechanical arm.
        self._clear_movement_state(send_stop=False)
        self._send_simple("stop")
        self._send_simple("arm stop")
        with self.udp_lock:
            self.udp_gamepad_active = False
            self.udp_failsafe_sent = True
        self.udp_last_debug_log_time = 0.0

    def _cancel_release_job(self, key: str):
        job = self.release_jobs.pop(key, None)
        if job is not None:
            try:
                self.after_cancel(job)
            except Exception:
                pass

    def _heartbeat_loop(self):
        while self.heartbeat_running:
            time.sleep(self.HEARTBEAT_INTERVAL_S)
            state, cmd = self._get_movement_snapshot()
            if state in {"forward", "backward", "left", "right"} and cmd and self.serial_mgr.is_connected():
                try:
                    self.serial_mgr.send_line(cmd)
                except Exception as exc:
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
                self._send_simple("stop")
            return
        key_to_motion = {"w": "forward", "s": "backward", "a": "left", "d": "right"}
        desired_state = key_to_motion[chosen_key]
        desired_cmd = self._build_movement_command(desired_state)
        if current_state != desired_state or current_cmd != desired_cmd:
            self._set_movement_state(desired_state, desired_cmd)
            self._send_line(desired_cmd)

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
            self._adjust_pwm(self._get_pwm_step())
            return
        if key in {"minus", "underscore", "kp_subtract"}:
            self._adjust_pwm(-self._get_pwm_step())
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
            self.release_jobs[key] = self.after(self.RELEASE_DEBOUNCE_MS, lambda k=key: self._finalize_movement_release(k))

    def _finalize_movement_release(self, key: str):
        self.release_jobs.pop(key, None)
        if key not in self.key_states:
            return
        self.key_states[key] = False
        self.press_order[key] = 0
        self._recompute_movement_state()

    def _send_motion(self, cmd: str):
        motion_cmd = self._build_movement_command(cmd)
        self._set_movement_state(cmd, motion_cmd)
        self._send_line(motion_cmd)

    def _send_simple(self, cmd: str):
        self._send_line(cmd)

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

    def _parse_arm_telemetry(self, line: str) -> bool:
        tel_index = line.find("TEL |")
        if tel_index == -1:
            return False
        tel_line = line[tel_index:]
        parts = [p.strip() for p in tel_line.split("|")[1:]]
        for part in parts:
            tokens = part.split()
            if len(tokens) >= 2:
                raw_name = tokens[0]
                row_name = self.arm_name_alias.get(raw_name, raw_name)
                pwm_val = tokens[1].replace("P:", "")
                angle_val = "N/A"
                if len(tokens) >= 3 and tokens[2].startswith("A:"):
                    angle_val = tokens[2].replace("A:", "")
                if f"arm_{row_name}" in self.arm_table.get_children():
                    self.arm_table.item(f"arm_{row_name}", values=(row_name, pwm_val, angle_val))
        return True

    def _poll_queues(self):
        while not self.serial_mgr.rx_queue.empty():
            line = self.serial_mgr.rx_queue.get_nowait()
            if not self._parse_arm_telemetry(line):
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

    def _log(self, text: str):
        timestamp = time.strftime("%H:%M:%S")
        self.console.configure(state="normal")
        self.console.insert("end", f"[{timestamp}] {text}\n")
        self.console.see("end")
        self.console.configure(state="disabled")

    def _clear_console(self):
        self.console.configure(state="normal")
        self.console.delete("1.0", "end")
        self.console.configure(state="disabled")


if __name__ == "__main__":
    app = MotorControlGUI()
    app.mainloop()
