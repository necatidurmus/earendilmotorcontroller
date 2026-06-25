#!/usr/bin/env python3
"""
FTDI H7 Client — Shared serial communication layer for F411 motor controller.

Used by both ftdi_h7_emulator.py (CLI) and ftdi_h7_gui.py (GUI).

Handles:
  - opening/closing serial port
  - sending commands (thread-safe)
  - reading telemetry lines in a background thread
  - parsing telemetry into a dict
  - heartbeat / keepalive command sending
  - safe stop on close/error
  - CSV logging
"""

import csv
import os
import queue
import threading
import time

try:
    import serial
except ImportError:
    serial = None

DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200

TELEMETRY_KEYS = [
    "RPM", "D", "DIR", "APP_PH", "PH", "SP", "BRAKE", "FC", "H", "PWM_SET", "PWM_ACT",
]

PHASE_NAMES = {
    0: "STOPPED",
    1: "RUNNING",
    2: "BRAKE",
    3: "NEUTRAL",
    4: "FAULT",
}


def parse_telemetry(line):
    """Parse a firmware telemetry line into a dict.

    Example input:
        RPM:23,T:23,D:67,DIR:F,APP_PH:2,SP:1,BRAKE:0,FC:0,H:5,PWM_SET:67,PWM_ACT:67
    """
    telem = {}
    try:
        for part in line.split(","):
            if ":" not in part:
                continue
            key, val = part.split(":", 1)
            telem[key] = val
    except (ValueError, IndexError):
        pass
    return telem


def _safe_int(val, default=0):
    try:
        return int(val)
    except (ValueError, TypeError):
        return default


def telem_to_display(telem):
    """Convert raw telemetry dict to typed display dict."""
    d = {}
    d["rpm"] = _safe_int(telem.get("RPM", 0))
    target_str = telem.get("T", telem.get("Tcmd", ""))
    d["target"] = _safe_int(target_str) if target_str else 0
    d["duty"] = _safe_int(telem.get("D", 0))
    d["dir_str"] = telem.get("DIR", "-")
    d["direction"] = {"F": 1, "R": -1}.get(d["dir_str"], 0)
    d["phase"] = _safe_int(telem.get("APP_PH", telem.get("PH", 0)))
    d["phase_name"] = PHASE_NAMES.get(d["phase"], "?")
    d["speed_mode"] = telem.get("SP", "0") == "1"
    d["brake"] = telem.get("BRAKE", "0") == "1"
    d["fault_code"] = _safe_int(telem.get("FC", 0))
    d["hall"] = _safe_int(telem.get("H", 0))
    d["pwm_set"] = _safe_int(telem.get("PWM_SET", telem.get("D", 0)))
    d["pwm_act"] = _safe_int(telem.get("PWM_ACT", telem.get("D", 0)))
    return d


class FtdiH7Client:
    """Thread-safe serial client for the F411 motor controller.

    Lifecycle:
        client = FtdiH7Client(port, baud)
        client.connect()
        client.send("hall")
        ...
        client.safe_stop()
        client.disconnect()

    The background reader thread puts (timestamp, raw_line, parsed_dict) tuples
    into ``client.telemetry_queue`` and also updates ``client.latest_display``
    for quick GUI access.
    """

    def __init__(self, port=DEFAULT_PORT, baud=DEFAULT_BAUD):
        self.port = port
        self.baud = baud
        self.ser = None
        self._send_lock = threading.Lock()
        self._running = False
        self._reader_thread = None
        self._heartbeat_thread = None

        # Telemetry state
        self.latest_display = {}
        self.latest_raw_line = ""
        self.telemetry_queue = queue.Queue(maxsize=500)
        self.all_lines_queue = queue.Queue(maxsize=2000)

        # Heartbeat state
        self._heartbeat_cmd = None  # command string or None
        self._heartbeat_interval = 0.4  # seconds
        self._heartbeat_event = threading.Event()

        # CSV logging
        self._csv_file = None
        self._csv_writer = None
        self._csv_path = None
        self._csv_lock = threading.Lock()
        self._log_start_time = None

        # Connection state
        self.connected = False
        self.error_msg = ""
        self.cmd_count = 0
        self.telem_count = 0

    # ── Connection ──────────────────────────────────────────────────

    def connect(self):
        if serial is None:
            raise ImportError("pyserial not installed: pip install pyserial")
        self.ser = serial.Serial(self.port, self.baud, timeout=0.05)
        time.sleep(0.1)
        self.ser.reset_input_buffer()
        self.connected = True
        self._running = True
        self._start_reader()
        self._start_heartbeat()

    def disconnect(self):
        self._running = False
        self._heartbeat_event.set()
        self._heartbeat_cmd = None
        if self._reader_thread and self._reader_thread.is_alive():
            self._reader_thread.join(timeout=2.0)
        if self._heartbeat_thread and self._heartbeat_thread.is_alive():
            self._heartbeat_thread.join(timeout=2.0)
        self.stop_csv_log()
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass
        self.connected = False

    # ── Send ────────────────────────────────────────────────────────

    def send(self, cmd):
        if not self.ser or not self.ser.is_open:
            return False
        try:
            with self._send_lock:
                self.ser.write(f"{cmd}\n".encode("utf-8"))
            self.cmd_count += 1
            return True
        except (serial.SerialException, OSError):
            self.error_msg = "Serial write error"
            return False

    # ── Safe stop ───────────────────────────────────────────────────

    def safe_stop(self):
        """Send emergency stop sequence: rpm 0, s, mode duty.

        (Was "mode normal" in the legacy Arduino firmware; the cube
        firmware accepts both `mode duty` and `mode normal` as aliases
        — ISSUE-037 — but the canonical cube name is `mode duty`.)
        """
        self.clear_heartbeat()
        self.send("rpm 0")
        self.send("s")
        self.send("mode duty")

    def stop_only(self):
        """Send coast stop only."""
        self.send("s")

    def brake(self):
        """Send brake command."""
        self.send("x")

    # ── Heartbeat ───────────────────────────────────────────────────

    def set_heartbeat(self, cmd, interval=0.4):
        """Set a repeating heartbeat command (e.g. 'f', 'b', 'rpm 100')."""
        self._heartbeat_cmd = cmd
        self._heartbeat_interval = interval
        self._heartbeat_event.clear()

    def clear_heartbeat(self):
        """Stop heartbeat sending."""
        self._heartbeat_cmd = None
        self._heartbeat_event.set()

    def _start_heartbeat(self):
        self._heartbeat_thread = threading.Thread(
            target=self._heartbeat_loop, daemon=True
        )
        self._heartbeat_thread.start()

    def _heartbeat_loop(self):
        while self._running:
            cmd = self._heartbeat_cmd
            if cmd:
                self.send(cmd)
                # Sleep in small increments so we can react to changes
                deadline = time.time() + self._heartbeat_interval
                while time.time() < deadline and self._running:
                    time.sleep(0.05)
                    if self._heartbeat_cmd != cmd:
                        break
            else:
                self._heartbeat_event.wait(timeout=0.1)
                self._heartbeat_event.clear()

    # ── Background reader ───────────────────────────────────────────

    def _start_reader(self):
        self._reader_thread = threading.Thread(
            target=self._reader_loop, daemon=True
        )
        self._reader_thread.start()

    def _reader_loop(self):
        while self._running:
            try:
                if self.ser and self.ser.in_waiting > 0:
                    raw = self.ser.readline()
                    line = raw.decode("utf-8", errors="ignore").strip()
                    if not line:
                        continue
                    try:
                        self.all_lines_queue.put_nowait(line)
                    except queue.Full:
                        pass
                    self.latest_raw_line = line
                    if line.startswith("RPM:"):
                        telem = parse_telemetry(line)
                        display = telem_to_display(telem)
                        self.latest_display = display
                        self.telem_count += 1
                        try:
                            self.telemetry_queue.put_nowait((time.time(), line, display))
                        except queue.Full:
                            pass
                        self._write_csv_row(time.time(), display, line)
                    else:
                        # Non-telemetry line (OK, MODE, etc.)
                        self._write_csv_row(time.time(), {}, line)
                else:
                    time.sleep(0.01)
            except (serial.SerialException, OSError):
                self.error_msg = "Serial connection lost"
                self.connected = False
                break
            except Exception:
                time.sleep(0.01)

    # ── CSV logging ─────────────────────────────────────────────────

    def start_csv_log(self, path=None):
        if path is None:
            ts = time.strftime("%Y%m%d_%H%M%S")
            path = f"telemetry_{ts}.csv"
        with self._csv_lock:
            if self._csv_file:
                return self._csv_path
            try:
                f = open(path, "w", newline="")
                writer = csv.writer(f)
                writer.writerow([
                    "time_s", "rpm", "target", "duty", "dir", "phase",
                    "speed_mode", "brake", "fault", "hall", "raw_line",
                ])
                self._csv_file = f
                self._csv_writer = writer
                self._csv_path = path
                self._log_start_time = time.time()
                return path
            except OSError as e:
                self.error_msg = f"CSV log error: {e}"
                return None

    def stop_csv_log(self):
        with self._csv_lock:
            if self._csv_file:
                try:
                    self._csv_file.close()
                except Exception:
                    pass
                self._csv_file = None
                self._csv_writer = None

    def _write_csv_row(self, ts, display, raw_line):
        with self._csv_lock:
            if not self._csv_writer:
                return
            try:
                elapsed = round(ts - self._log_start_time, 3) if self._log_start_time else 0
                self._csv_writer.writerow([
                    elapsed,
                    display.get("rpm", ""),
                    display.get("target", ""),
                    display.get("duty", ""),
                    display.get("dir_str", ""),
                    display.get("phase_name", ""),
                    int(display.get("speed_mode", False)),
                    int(display.get("brake", False)),
                    display.get("fault_code", ""),
                    display.get("hall", ""),
                    raw_line,
                ])
            except Exception:
                pass

    @property
    def csv_log_path(self):
        return self._csv_path

    @property
    def csv_logging(self):
        return self._csv_file is not None

    # ── Drain helpers (for CLI use) ─────────────────────────────────

    def drain_until_timeout(self, timeout, stop_event=None):
        """Read and print all lines until timeout. Returns list of lines."""
        lines = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            if stop_event and stop_event.is_set():
                break
            try:
                line = self.all_lines_queue.get(timeout=0.05)
                lines.append(line)
                print(line)
            except queue.Empty:
                pass
        return lines

    def drain_until_pattern(self, timeout, patterns=None, stop_event=None):
        """Read lines until a pattern matches or timeout."""
        lines = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            if stop_event and stop_event.is_set():
                break
            try:
                line = self.all_lines_queue.get(timeout=0.05)
                lines.append(line)
                print(line)
                if patterns:
                    for p in patterns:
                        if p in line:
                            return lines
            except queue.Empty:
                pass
        return lines

    def drain_all(self):
        """Drain and return all pending non-telemetry lines."""
        out = []
        while True:
            try:
                out.append(self.all_lines_queue.get_nowait())
            except queue.Empty:
                break
        return out
