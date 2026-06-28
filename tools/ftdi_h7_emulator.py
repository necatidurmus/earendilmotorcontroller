#!/usr/bin/env python3
"""
FTDI H7 Emulator — BLDC Motor Service & Motion Test Tool
==========================================================
F411 motor sürücü modülü için hem hareket testi hem de servis/diagnostik
komutlarını çalıştıran PC-side CLI aracı.

Hareket modları: duty, speed
Servis komutları: hall, status, spstat, map, mapreset, save, reload,
                  scan, test, identify, clrerr, raw --cmd

Kullanım:
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --hall
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --status
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --scan
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --test
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --identify
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --identify --save-map
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --cmd "spstat" --listen-time 2
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --mode duty --rpm 500
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --mode speed --rpm 500
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --mode speed --rpm 500 --kp 1.2 --ki 0.15
    python3 tools/ftdi_h7_emulator.py --port /dev/ttyUSB0 --pi-step 200,400,600,800 --step-duration 5
"""

import signal
import sys
import time
import argparse
import threading

from ftdi_h7_client import DEFAULT_PORT, DEFAULT_BAUD, FtdiH7Client, parse_telemetry

# Servis komut timeoutları (saniye)
SERVICE_TIMEOUTS = {
    "hall": 2.0,
    "status": 2.0,
    "spstat": 2.0,
    "map": 2.0,
    "mapreset": 2.0,
    "reload": 2.0,
    "save": 2.0,
    "clrerr": 2.0,
    "scan": 15.0,
    "test": 20.0,
    "identify": 30.0,
}

HEARTBEAT_INTERVAL = 0.5


# ─── Servis Komutları ─────────────────────────────────────────────
def cmd_simple(client, command, timeout):
    print(f">> {command}")
    client.send(command)
    lines = client.drain_until_timeout(timeout)
    has_error = any("[ERR]" in l for l in (lines or []))
    if has_error:
        print("[DONE] Firmware hata döndürdü (yukarıya bakın)")
    else:
        print("[OK] Tamamlandı")


def cmd_scan(client, timeout, stop_event):
    print(">> scan")
    print("[INFO] Scan çalışıyor... Ctrl+C ile durdurulabilir.")
    client.send("scan")
    client.drain_until_timeout(timeout, stop_event=stop_event)
    if stop_event.is_set():
        client.send("s")
        print("\n[WARN] Scan iptal edildi (Ctrl+C)")
    else:
        print("[OK] Scan tamamlandı")


def cmd_test(client, timeout, stop_event):
    print(">> test")
    print("[INFO] Test çalışıyor... Ctrl+C ile durdurulabilir.")
    client.send("test")
    client.drain_until_timeout(timeout, stop_event=stop_event)
    if stop_event.is_set():
        client.send("s")
        print("\n[WARN] Test iptal edildi (Ctrl+C)")
    else:
        print("[OK] Test tamamlandı")


def cmd_identify(client, timeout, stop_event, save_map=False):
    print(">> identify")
    print("[INFO] Identify çalışıyor... Ctrl+C ile durdurulabilir.")
    client.send("identify")
    client.drain_until_pattern(
        timeout,
        patterns=["[OK]", "[ERR]", "[FAIL]", "Done", "done", "complete", "Complete"],
        stop_event=stop_event,
    )
    if stop_event.is_set():
        client.send("s")
        print("\n[WARN] Identify iptal edildi (Ctrl+C)")
        return

    if save_map:
        print("\n>> save (identify sonrası harita kaydediliyor)")
        print("[WARN] Persistent storage is disabled in current firmware; save will return an error.")
        time.sleep(0.2)
        client.send("save")
        client.drain_until_timeout(2.0)
        print("\n>> map (son harita)")
        client.send("map")
        client.drain_until_timeout(2.0)

    print("[OK] Identify tamamlandı")


# ─── Hareket Modu ─────────────────────────────────────────────────
def configure_pi_params(client, args):
    cmds = []
    if args.kp is not None and args.ki is not None:
        cmds.append(f"pi {args.kp} {args.ki}")
    elif args.kp is not None:
        cmds.append(f"kp {args.kp}")
    elif args.ki is not None:
        cmds.append(f"ki {args.ki}")

    if args.base:
        cmds.append(f"base {args.base[0]} {args.base[1]} {args.base[2]}")

    if args.boost:
        cmds.append(f"boost {args.boost[0]} {args.boost[1]} {args.boost[2]} {args.boost[3]}")

    if args.ramp_up is not None or args.ramp_down is not None:
        up = args.ramp_up if args.ramp_up is not None else 1000
        down = args.ramp_down if args.ramp_down is not None else 1000
        cmds.append(f"ramp {up} {down}")

    for c in cmds:
        print(f">> {c}")
        client.send(c)
        time.sleep(0.1)
        for line in client.drain_all():
            print(f"   {line}")


def motion_loop(client, mode, rpm, duration, stop_event):
    if mode == "speed":
        print(f"[INFO] Speed modu: hedef RPM={rpm}, süre={duration}s")
        client.send("mode speed")
        time.sleep(0.3)
        client.send(f"rpm {rpm}")
        client.set_heartbeat(f"rpm {rpm}", interval=HEARTBEAT_INTERVAL)
    else:
        duty = max(0, min(4000, rpm))
        print(f"[INFO] Duty modu: duty={duty}, süre={duration}s")
        client.send(f"f{duty}")
        client.set_heartbeat(f"f{duty}", interval=HEARTBEAT_INTERVAL)

    deadline = time.time() + duration
    telem_count = 0

    while time.time() < deadline and not stop_event.is_set():
        try:
            line = client.all_lines_queue.get(timeout=0.05)
            if line.startswith("RPM:"):
                telem_count += 1
            print(line)
        except Exception:
            pass

    client.clear_heartbeat()

    if mode == "speed":
        client.send("rpm 0")
        time.sleep(0.05)
        client.send("mode duty")
        time.sleep(0.05)
    client.send("s")
    time.sleep(0.05)
    client.send("mode duty")
    time.sleep(0.1)

    if stop_event.is_set():
        print(f"\n[WARN] Hareket iptal edildi (Ctrl+C). Telemetri: {telem_count}")
    else:
        print(f"[OK] Hareket tamamlandı. Telemetri: {telem_count}")


def pi_step_test(client, rpm_targets, step_duration, stop_event):
    print(f"[INFO] PI Step Test: hedefler={rpm_targets}, adım süresi={step_duration}s")
    client.send("mode speed")
    time.sleep(0.3)

    all_data = []
    step_num = 0

    for target_rpm in rpm_targets:
        if stop_event.is_set():
            break

        step_num += 1
        print(f"\n── Step {step_num}/{len(rpm_targets)}: RPM={target_rpm} ──")
        client.send(f"rpm {target_rpm}")
        client.set_heartbeat(f"rpm {target_rpm}", interval=HEARTBEAT_INTERVAL)

        deadline = time.time() + step_duration
        step_data = []

        while time.time() < deadline and not stop_event.is_set():
            try:
                line = client.all_lines_queue.get(timeout=0.05)
                if line.startswith("RPM:"):
                    telem = parse_telemetry(line)
                    telem["_target"] = target_rpm
                    telem["_step"] = step_num
                    step_data.append(telem)
                print(f"  [{step_num}] {line}")
            except Exception:
                pass

        client.clear_heartbeat()
        all_data.extend(step_data)

        if step_data:
            rpms = [int(d.get("RPM", 0)) for d in step_data if d.get("RPM")]
            if rpms:
                print(f"  Step {step_num} özet: min={min(rpms)} max={max(rpms)} son={rpms[-1]}")

    # Durdur
    client.clear_heartbeat()
    client.send("rpm 0")
    time.sleep(0.05)
    client.send("mode duty")
    time.sleep(0.05)
    client.send("s")
    time.sleep(0.05)
    client.send("mode duty")
    time.sleep(0.1)

    if stop_event.is_set():
        print(f"\n[WARN] PI Step Test iptal edildi (Ctrl+C).")
    else:
        print(f"\n[OK] PI Step Test tamamlandı. Toplam telemetri: {len(all_data)}")

    if all_data:
        print("\n── Özet ──")
        for step in range(1, step_num + 1):
            sd = [d for d in all_data if d.get("_step") == step]
            if not sd:
                continue
            rpms = [int(d.get("RPM", 0)) for d in sd if d.get("RPM")]
            target = int(sd[0].get("_target", 0))
            if rpms:
                if target > 0:
                    overshoot = max(0, max(rpms) - target)
                elif target < 0:
                    overshoot = max(0, min(rpms) - target)
                else:
                    overshoot = 0
                print(f"  Step {step} (hedef={target}): min={min(rpms)} max={max(rpms)} overshoot={overshoot}")


# ─── Ana ──────────────────────────────────────────────────────────
def build_parser():
    p = argparse.ArgumentParser(
        description="FTDI H7 Emulator — BLDC Motor Service & Motion Test Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--port", default=DEFAULT_PORT, help=f"Seri port (varsayılan: {DEFAULT_PORT})")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Baud rate (varsayılan: {DEFAULT_BAUD})")

    g = p.add_argument_group("Servis Komutları")
    g.add_argument("--hall", action="store_true", help="Hall sensör oku")
    g.add_argument("--status", action="store_true", help="Motor durumu")
    g.add_argument("--spstat", action="store_true", help="Speed PI durumu")
    g.add_argument("--map", action="store_true", help="Hall haritasını göster")
    g.add_argument("--mapreset", action="store_true", help="Hall haritasını sıfırla")
    g.add_argument("--reload", action="store_true", help="Hall haritasını EEPROM'dan yükle")
    g.add_argument("--save", action="store_true", help="Hall haritasını EEPROM'a kaydet")
    g.add_argument("--scan", action="store_true", help="Hall tarama")
    g.add_argument("--test", action="store_true", help="Faz testi")
    g.add_argument("--identify", action="store_true", help="Hall harita otomatik algılama")
    g.add_argument("--clrerr", action="store_true", help="Hata bayraklarını temizle")
    g.add_argument("--save-map", action="store_true", help="Identify sonrası haritayı kaydet")
    g.add_argument("--cmd", type=str, help="Ham komut gönder")

    g2 = p.add_argument_group("Hareket Testi")
    g2.add_argument("--mode", choices=["duty", "speed"], help="Hareket modu")
    g2.add_argument("--rpm", type=int, default=None, help="Hedef RPM (speed) veya duty 0-4000 (duty)")
    g2.add_argument("--duration", type=int, default=5, help="Test süresi saniye (varsayılan: 5)")

    g4 = p.add_argument_group("Speed PI Parametreleri (--mode speed ile)")
    g4.add_argument("--kp", type=float, help="PI Kp kazancı (0-10)")
    g4.add_argument("--ki", type=float, help="PI Ki kazancı (0-10)")
    g4.add_argument("--base", type=int, nargs=3, metavar=("LOW", "MID", "HIGH"), help="Base PWM değerleri")
    g4.add_argument("--boost", type=int, nargs=4, metavar=("LOW", "MID", "HIGH", "MS"), help="Start boost")
    g4.add_argument("--ramp-up", type=float, help="Ramp yukarı hızı (RPM/s)")
    g4.add_argument("--ramp-down", type=float, help="Ramp aşağı hızı (RPM/s)")
    g4.add_argument("--pi-step", type=str, help="Step test RPM hedefleri (virgülle: 200,400,600,800)")
    g4.add_argument("--step-duration", type=int, default=5, help="Her step süresi saniye")

    g3 = p.add_argument_group("Ortak Seçenekler")
    g3.add_argument("--timeout", type=float, help="Servis komut timeout (saniye)")
    g3.add_argument("--listen-time", type=float, help="--cmd için dinleme süresi (saniye)")

    return p


def main():
    parser = build_parser()
    args = parser.parse_args()

    service_flags = [
        args.hall, args.status, args.spstat, args.map, args.mapreset,
        args.reload, args.save, args.scan, args.test, args.identify,
        args.clrerr, args.cmd is not None,
    ]
    has_service = any(service_flags)
    has_motion = args.mode is not None
    has_pi_step = args.pi_step is not None
    has_pi_params = any([
        args.kp is not None, args.ki is not None,
        args.base is not None, args.boost is not None,
        args.ramp_up is not None, args.ramp_down is not None,
    ])

    if (has_motion or has_pi_step) and has_service:
        print("[ERR] Do not combine motion tests with service commands.")
        sys.exit(1)

    if args.save_map and not args.identify:
        print("[ERR] --save-map requires --identify.")
        sys.exit(1)

    if has_pi_params and not has_motion and not has_pi_step:
        print("[ERR] PI parameters require --mode speed or --pi-step.")
        sys.exit(1)

    if has_pi_step and has_motion:
        print("[ERR] --pi-step is its own mode. Do not combine with --mode.")
        sys.exit(1)

    if has_pi_step and args.rpm is not None:
        print("[ERR] --pi-step uses its own RPM targets. Remove --rpm.")
        sys.exit(1)

    rpm = args.rpm if args.rpm is not None else 500

    if not has_motion and not has_service and not has_pi_step:
        parser.print_help()
        sys.exit(0)

    client = FtdiH7Client(args.port, args.baud)
    try:
        client.connect()
    except Exception as e:
        print(f"[ERR] Serial açılamadı: {e}")
        sys.exit(1)

    stop_event = threading.Event()

    def sigint_handler(sig, frame):
        stop_event.set()
    old_handler = signal.signal(signal.SIGINT, sigint_handler)

    try:
        if args.hall:
            t = args.timeout or SERVICE_TIMEOUTS["hall"]
            cmd_simple(client, "hall", t)
        elif args.status:
            t = args.timeout or SERVICE_TIMEOUTS["status"]
            cmd_simple(client, "status", t)
        elif args.spstat:
            t = args.timeout or SERVICE_TIMEOUTS["spstat"]
            cmd_simple(client, "spstat", t)
        elif args.map:
            t = args.timeout or SERVICE_TIMEOUTS["map"]
            cmd_simple(client, "map", t)
        elif args.mapreset:
            t = args.timeout or SERVICE_TIMEOUTS["mapreset"]
            cmd_simple(client, "mapreset", t)
        elif args.reload:
            t = args.timeout or SERVICE_TIMEOUTS["reload"]
            cmd_simple(client, "reload", t)
        elif args.save:
            t = args.timeout or SERVICE_TIMEOUTS["save"]
            cmd_simple(client, "save", t)
        elif args.clrerr:
            t = args.timeout or SERVICE_TIMEOUTS["clrerr"]
            cmd_simple(client, "clrerr", t)
        elif args.scan:
            t = args.timeout or SERVICE_TIMEOUTS["scan"]
            cmd_scan(client, t, stop_event)
        elif args.test:
            t = args.timeout or SERVICE_TIMEOUTS["test"]
            cmd_test(client, t, stop_event)
        elif args.identify:
            t = args.timeout or SERVICE_TIMEOUTS["identify"]
            cmd_identify(client, t, stop_event, save_map=args.save_map)
        elif args.cmd:
            t = args.timeout or args.listen_time or 2.0
            print(f">> {args.cmd}")
            client.send(args.cmd)
            client.drain_until_timeout(t)
            print("[OK] Tamamlandı")

        elif has_pi_step:
            rpm_targets = [int(x.strip()) for x in args.pi_step.split(",")]
            if not rpm_targets:
                print("[ERR] --pi-step requires at least one RPM target.")
                sys.exit(1)
            configure_pi_params(client, args)
            pi_step_test(client, rpm_targets, args.step_duration, stop_event)

        elif has_motion:
            if has_pi_params:
                configure_pi_params(client, args)
            motion_loop(client, args.mode, rpm, args.duration, stop_event)

    except KeyboardInterrupt:
        stop_event.set()
        print("\n[WARN] Ctrl+C — motor durduruluyor...")
        client.send("s")
        time.sleep(0.05)
        client.send("mode duty")
        time.sleep(0.1)
    finally:
        client.safe_stop()
        time.sleep(0.05)
        client.disconnect()
        signal.signal(signal.SIGINT, old_handler)


if __name__ == "__main__":
    main()
