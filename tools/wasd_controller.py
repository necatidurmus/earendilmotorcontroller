#!/usr/bin/env python3
"""
BLDC Motor f/b/s Controller — Hold-to-Run PWM Hız Kontrolü
============================================================
Firmware'da "mode control" aktifken çalışır.
Motor F/B tuşuna basılı tutunca döner, bırakınca durur.

NOT: Bu sürüm curses tabanlıdır ve gerçek key-release event'i yakalayamaz.
Hold-to-run mantığı polling + timeout ile çalışır:
- Tuş basılıyken: her 500ms'de bir f/b heartbeat gönderilir (lease yenileme)
- Tuş bırakılınca: ~240ms içinde no-key algılanırsa stop gönderilir
- Bu, motor_gui.py'nin gerçek key-release yakalamasından farklıdır

Tuşlar:
  F → İleri (basılı tut — bırakınca durur)
  B → Geri  (basılı tut — bırakınca durur)
  D / ↑ → PWM artır (+10) — hız yükselir
  A / ↓ → PWM azalt (-10) — hız düşer
  S → Motoru durdur (coast)
  Q → Çıkış (motor durur, normal moda döner)

Güvenlik özellikleri:
- Heartbeat sadece CONTROL modunda ve onaylanmışsa çalışır
- Host connection timeout (2sn UART aktivite yoksa stop)
- Command watchdog (800ms lease timeout)

Kullanım:
    python3 wasd_controller.py                    # varsayılan /dev/ttyUSB0
    python3 wasd_controller.py /dev/ttyACM0       # port belirt
    python3 wasd_controller.py /dev/ttyUSB0 115200 # port + baud
"""

import sys
import time
import threading
import curses

try:
    import serial
except ImportError:
    print("pyserial gerekli: pip install pyserial")
    sys.exit(1)

# ─── Ayarlar ─────────────────────────────────────────────────────
DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200

PWM_STEP = 10
RPM_MAX_DISPLAY = 2000  # RPM bar max değeri


# ─── Motor Controller ────────────────────────────────────────────
class MotorController:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        time.sleep(0.1)

        # Durum (firmware telemetrisinden güncellenir)
        self.rpm = 0
        self.duty = 0
        self.pwm_set = 150
        self.pwm_act = 0
        self.direction = 0  # 0=durdu, 1=ileri, -1=geri
        self.fw_direction = 0
        self.dir_str = "-"
        self.phase = 0
        self.phase_name = "STOPPED"

        self.running = True
        self.mode_confirmed = False
        self.current_mode = "NORMAL"  # "NORMAL" | "CONTROL" | "SETTINGS"

        # Hold-to-run (gerçek key-release değil, timeout bazlı polling)
        # curses nodelay modunda tuş bırakma eventi yok, bu yüzden
        # heartbeat + timeout ile 'basılı tutma' simüle edilir
        self.key_held = None  # None, 'f', veya 'b'
        self._no_key_count = 0
        self._send_lock = threading.Lock()

        # İstatistik
        self.cmd_count = 0
        self.telem_count = 0
        self.last_response = ""
        self.error_msg = ""

        self.ser.reset_input_buffer()

    def send_command(self, cmd):
        try:
            with self._send_lock:
                self.ser.write(f"{cmd}\n".encode("utf-8"))
            self.cmd_count += 1
        except serial.SerialException:
            pass

    def activate_control_mode(self):
        """Firmware'a Control moduna geçmesini söyle"""
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
        """Çıkışta normal moda dön"""
        self.send_command("s")
        time.sleep(0.05)
        self.send_command("mode normal")
        time.sleep(0.1)

    def forward(self):
        """F — ileri başlat, durdurana kadar çalışır"""
        self.direction = 1
        self.send_command("f")

    def backward(self):
        """B — geri başlat, durdurana kadar çalışır"""
        self.direction = -1
        self.send_command("b")

    def stop(self):
        """S — motoru durdur (coast)"""
        self.direction = 0
        self.send_command("s")

    def brake(self):
        """Space — aktif fren (brake)"""
        self.direction = 0
        self.send_command("x")

    def pwm_up(self):
        """D/↑ — PWM artır, hız yükselir"""
        self.pwm_set = min(255, self.pwm_set + PWM_STEP)
        self.send_command(f"pwm {self.pwm_set}")

    def pwm_down(self):
        """A/↓ — PWM azalt, hız düşer"""
        self.pwm_set = max(0, self.pwm_set - PWM_STEP)
        self.send_command(f"pwm {self.pwm_set}")

    def set_key(self, key):
        """Hold-to-run: F/B basıldığında çağrılır. None = bırakıldı."""
        self.key_held = key
        if key in ("f", "b"):
            self.send_command(key)

    def _sender_loop(self):
        """Heartbeat: Control modunda ve key_held aktifken 0.5s'de bir komut tekrar gönder.
        Sadece mode_confirmed=True ve CONTROL modundayken çalışır (güvenlik)."""
        while self.running:
            # Güvenlik: Sadece CONTROL modunda ve onaylanmışsa heartbeat gönder
            if (
                self.current_mode == "CONTROL"
                and self.mode_confirmed
                and self.key_held in ("f", "b")
            ):
                self.send_command(self.key_held)
                time.sleep(0.5)
            else:
                time.sleep(0.03)

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
                        if "CONTROL" in line:
                            self.mode_confirmed = True
                            self.current_mode = "CONTROL"
                        elif "SETTINGS" in line:
                            self.mode_confirmed = False
                            self.current_mode = "SETTINGS"
                        elif "NORMAL" in line:
                            self.mode_confirmed = False
                            self.current_mode = "NORMAL"
                    else:
                        self.last_response = line
                else:
                    time.sleep(0.01)
            except (serial.SerialException, OSError):
                self.error_msg = "Serial bağlantı hatası!"
                time.sleep(0.1)
            except Exception:
                time.sleep(0.01)

    def _parse_ok(self, line):
        """OK:FWD,PWM:60 veya OK:STOP gibi cevapları parse et"""
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
        except (ValueError, IndexError):
            pass

    def _parse_telemetry(self, line):
        """RPM:0,D:0,DIR:F,PH:0,PWM_SET:60,PWM_ACT:0,PWM:60,PDIR:0"""
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
                    # Backward compat: PWM_SET yoksa PWM kullan
                    if "PWM_SET" not in line:
                        self.pwm_set = int(val)
                elif key == "PDIR":
                    self.fw_direction = int(val)
                    self.direction = self.fw_direction
        except (ValueError, IndexError):
            pass

    def close(self):
        """Güvenli kapanış: önce motoru durdur, sonra normal moda dön."""
        self.key_held = None  # Heartbeat'ı durdur
        self.running = False
        try:
            # Önce motoru durdur (coast)
            self.stop()
            time.sleep(0.05)
            # Sonra normal moda dön
            self.restore_normal_mode()
            time.sleep(0.1)
            self.ser.close()
        except Exception:
            pass


# ─── UI ───────────────────────────────────────────────────────────
def draw_bar(stdscr, row, col, value, max_val, bar_len, fill_color, empty_attr=None):
    """Yatay bar çiz"""
    pct = min(1.0, max(0.0, value / max_val)) if max_val > 0 else 0
    filled = int(pct * bar_len)
    empty = bar_len - filled

    try:
        stdscr.addstr(row, col, "█" * filled, fill_color)
        if empty_attr:
            stdscr.addstr(row, col + filled, "░" * empty, empty_attr)
        else:
            stdscr.addstr(row, col + filled, "░" * empty)
    except curses.error:
        pass


def draw_ui(stdscr, ctrl, port):
    h, w = stdscr.getmaxyx()

    curses.init_pair(1, curses.COLOR_GREEN, curses.COLOR_BLACK)
    curses.init_pair(2, curses.COLOR_RED, curses.COLOR_BLACK)
    curses.init_pair(3, curses.COLOR_YELLOW, curses.COLOR_BLACK)
    curses.init_pair(4, curses.COLOR_CYAN, curses.COLOR_BLACK)
    curses.init_pair(5, curses.COLOR_WHITE, curses.COLOR_BLUE)
    curses.init_pair(6, curses.COLOR_MAGENTA, curses.COLOR_BLACK)
    curses.init_pair(7, curses.COLOR_BLACK, curses.COLOR_GREEN)
    curses.init_pair(8, curses.COLOR_BLACK, curses.COLOR_RED)

    GREEN = curses.color_pair(1)
    RED = curses.color_pair(2)
    YELLOW = curses.color_pair(3)
    CYAN = curses.color_pair(4)
    HEADER = curses.color_pair(5) | curses.A_BOLD
    MAGENTA = curses.color_pair(6)
    STATUS_OK = curses.color_pair(7) | curses.A_BOLD
    STATUS_ERR = curses.color_pair(8) | curses.A_BOLD

    try:
        stdscr.erase()

        # ═══ Başlık ═══
        title = " ⚡ BLDC Motor — Hold-to-Run Kontrol ⚡ "
        pad = max(0, (w - len(title)) // 2)
        stdscr.addstr(0, 0, " " * min(w, 60), HEADER)
        stdscr.addstr(0, pad, title[: w - 1], HEADER)

        # Bağlantı durumu
        stdscr.addstr(1, 2, f"Port: {port}", CYAN)
        col = 2 + len(f"Port: {port}") + 2
        if ctrl.mode_confirmed:
            stdscr.addstr(1, col, " CONTROL ", STATUS_OK)
        else:
            stdscr.addstr(1, col, " BEKLE.. ", STATUS_ERR)

        # ═══ Kontroller ═══
        stdscr.addstr(3, 2, "── Kontroller ──", curses.A_BOLD | curses.A_DIM)
        stdscr.addstr(4, 4, "[F]", GREEN | curses.A_BOLD)
        stdscr.addstr(4, 8, "İleri (basılı tut)")
        stdscr.addstr(4, 28, "[B]", RED | curses.A_BOLD)
        stdscr.addstr(4, 32, "Geri (basılı tut)")
        stdscr.addstr(5, 4, "[D/↑]", YELLOW | curses.A_BOLD)
        stdscr.addstr(5, 10, "Hız +10")
        stdscr.addstr(5, 24, "[A/↓]", YELLOW | curses.A_BOLD)
        stdscr.addstr(5, 30, "Hız -10")
        stdscr.addstr(6, 4, "[S]", MAGENTA | curses.A_BOLD)
        stdscr.addstr(6, 8, "DUR (coast)")
        stdscr.addstr(6, 28, "[SPACE]", MAGENTA | curses.A_BOLD)
        stdscr.addstr(6, 36, "FREN (brake)")
        stdscr.addstr(7, 4, "[Q]", curses.A_BOLD)
        stdscr.addstr(7, 8, "Çıkış")
        stdscr.addstr(7, 28, "[I]", CYAN | curses.A_BOLD)
        stdscr.addstr(7, 32, "Identify")

        # ═══ RPM Göstergesi ═══
        stdscr.addstr(8, 2, "── RPM ──", curses.A_BOLD | curses.A_DIM)

        rpm_color = GREEN if ctrl.rpm > 0 else CYAN
        stdscr.addstr(9, 2, "RPM:", curses.A_BOLD)
        stdscr.addstr(9, 7, f"{ctrl.rpm:5d}", rpm_color | curses.A_BOLD)

        # RPM bar
        bar_len = min(30, w - 18)
        if bar_len > 0:
            stdscr.addstr(9, 14, "[")
            draw_bar(
                stdscr, 9, 15, ctrl.rpm, RPM_MAX_DISPLAY, bar_len, GREEN, curses.A_DIM
            )
            stdscr.addstr(9, 15 + bar_len, "]")

        # ═══ Yön & Durum ═══
        stdscr.addstr(11, 2, "── Motor ──", curses.A_BOLD | curses.A_DIM)

        stdscr.addstr(12, 2, "Yön: ", curses.A_BOLD)
        if ctrl.direction == 1:
            stdscr.addstr(12, 7, "▶▶▶ İLERİ ▶▶▶", GREEN | curses.A_BOLD)
        elif ctrl.direction == -1:
            stdscr.addstr(12, 7, "◀◀◀ GERİ  ◀◀◀", RED | curses.A_BOLD)
        else:
            stdscr.addstr(12, 7, "─── DURDU ───", YELLOW)

        stdscr.addstr(13, 2, "Faz: ", curses.A_BOLD)
        ph_color = (
            GREEN
            if ctrl.phase_name == "RUNNING"
            else (
                RED
                if ctrl.phase_name == "FAULT"
                else (
                    MAGENTA
                    if ctrl.phase_name == "BRAKE"
                    else (YELLOW if ctrl.phase_name == "KICK" else CYAN)
                )
            )
        )
        stdscr.addstr(13, 7, ctrl.phase_name, ph_color | curses.A_BOLD)

        # ═══ Hız (PWM) Kontrolü ═══
        stdscr.addstr(15, 2, "── Hız (PWM) ──", curses.A_BOLD | curses.A_DIM)

        stdscr.addstr(16, 2, "Set: ", curses.A_BOLD)
        stdscr.addstr(16, 7, f"{ctrl.pwm_set:3d}", CYAN | curses.A_BOLD)
        stdscr.addstr(16, 10, " /255")

        stdscr.addstr(17, 2, "Aktif:", curses.A_BOLD)
        stdscr.addstr(17, 8, f"{ctrl.pwm_act:3d}", YELLOW | curses.A_BOLD)
        stdscr.addstr(17, 11, " /255")

        # PWM bar
        if bar_len > 0:
            pct = ctrl.pwm_set / 255.0
            stdscr.addstr(18, 2, "[")
            draw_bar(stdscr, 18, 3, ctrl.pwm_set, 255, bar_len, YELLOW, curses.A_DIM)
            stdscr.addstr(18, 3 + bar_len, f"] {int(pct * 100):3d}%")

        # ═══ İstatistik ═══
        stdscr.addstr(
            20, 2, f"Cmd:{ctrl.cmd_count}  Tel:{ctrl.telem_count}", curses.A_DIM
        )

        if ctrl.error_msg:
            stdscr.addstr(21, 2, f"⚠ {ctrl.error_msg}", RED | curses.A_BOLD)
        elif ctrl.last_response:
            msg = ctrl.last_response[: min(w - 8, 45)]
            stdscr.addstr(21, 2, f"Son: {msg}", curses.A_DIM)

        stdscr.refresh()

    except curses.error:
        pass


def main(stdscr):
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(80)
    curses.use_default_colors()

    port = DEFAULT_PORT
    baud = DEFAULT_BAUD
    if len(sys.argv) > 1:
        port = sys.argv[1]
    if len(sys.argv) > 2:
        try:
            baud = int(sys.argv[2])
        except ValueError:
            pass

    try:
        ctrl = MotorController(port, baud)
    except serial.SerialException as e:
        stdscr.addstr(0, 0, f"Serial hata: {e}")
        stdscr.addstr(2, 0, "python3 wasd_controller.py [port] [baud]")
        stdscr.refresh()
        stdscr.timeout(-1)
        stdscr.getch()
        return

    # Firmware'da Control modunu aktifle
    ctrl.activate_control_mode()

    # Telemetri okuma thread'i
    telem_thread = threading.Thread(target=ctrl.read_telemetry_loop, daemon=True)
    telem_thread.start()

    # Heartbeat sender thread (hold-to-run)
    sender_thread = threading.Thread(target=ctrl._sender_loop, daemon=True)
    sender_thread.start()

    try:
        while True:
            draw_ui(stdscr, ctrl, port)

            key = stdscr.getch()

            if key == ord("q") or key == ord("Q"):
                break
            elif key == ord("f") or key == ord("F"):
                ctrl.set_key("f")
                ctrl._no_key_count = 0
            elif key == ord("b") or key == ord("B"):
                ctrl.set_key("b")
                ctrl._no_key_count = 0
            elif key == ord("d") or key == ord("D") or key == curses.KEY_UP:
                ctrl.pwm_up()
            elif key == ord("a") or key == ord("A") or key == curses.KEY_DOWN:
                ctrl.pwm_down()
            elif key == ord("s") or key == ord("S"):
                ctrl.set_key(None)
                ctrl.stop()
            elif key == ord(" "):
                ctrl.set_key(None)
                ctrl.brake()
            elif key == ord("i") or key == ord("I"):
                ctrl.send_command("identify")
            elif key == -1:
                # No key pressed — grace period: 3 consecutive (~240ms) before stop
                if ctrl.key_held is not None:
                    ctrl._no_key_count += 1
                    if ctrl._no_key_count >= 3:
                        ctrl.set_key(None)
                        ctrl.stop()
                        ctrl._no_key_count = 0
                else:
                    ctrl._no_key_count = 0

    except KeyboardInterrupt:
        pass
    finally:
        ctrl.close()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0)
    curses.wrapper(main)
