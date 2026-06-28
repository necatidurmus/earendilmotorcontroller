#!/usr/bin/env python3
"""F446 bridge smoke test.

Tests communication with F446 bridge firmware WITHOUT sending any motor
movement commands. Safe to run with motor connected or disconnected.

Usage:
    python tools/f446_serial_smoke_test.py --port /dev/ttyACM0
    python tools/f446_serial_smoke_test.py --port COM3 --baud 115200
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("ERROR: pyserial missing. Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)


PASS = 0
FAIL = 0
SKIP = 0


def report(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        msg = f"  FAIL  {name}"
        if detail:
            msg += f"  ({detail})"
        print(msg)


def report_skip(name, reason=""):
    global SKIP
    SKIP += 1
    msg = f"  SKIP  {name}"
    if reason:
        msg += f"  ({reason})"
    print(msg)


def send(ser, line):
    print(f"  TX: {line}")
    ser.write((line.strip() + "\n").encode("utf-8"))
    ser.flush()


def read_lines(ser, seconds):
    lines = []
    end = time.time() + seconds
    while time.time() < end:
        raw = ser.readline()
        if raw:
            text = raw.decode("utf-8", errors="replace").rstrip()
            if text:
                lines.append(text)
                print(f"  RX: {text}")
    return lines


def has_response(lines, expected_prefix):
    return any(expected_prefix in line for line in lines)


def main():
    ap = argparse.ArgumentParser(description="F446 bridge smoke test (no motor movement)")
    ap.add_argument("--port", required=True, help="Serial port (e.g. /dev/ttyACM0, COM3)")
    ap.add_argument("--baud", type=int, default=115200, help="Baud rate (default 115200)")
    args = ap.parse_args()

    print(f"Opening {args.port} at {args.baud} baud...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.3)
    except serial.SerialException as exc:
        print(f"ERROR: Cannot open port: {exc}", file=sys.stderr)
        sys.exit(1)

    with ser:
        print("Waiting for boot messages...")
        time.sleep(1.0)
        boot_lines = read_lines(ser, 1.0)

        print("\n--- Test 1: ping ---")
        send(ser, "ping")
        lines = read_lines(ser, 1.0)
        report("ping -> pong", has_response(lines, "pong"))

        print("\n--- Test 2: help ---")
        send(ser, "help")
        lines = read_lines(ser, 1.5)
        report("help -> command list", len(lines) >= 3)

        print("\n--- Test 3: bridge on ---")
        send(ser, "bridge on")
        lines = read_lines(ser, 1.0)
        report("bridge on -> OK", has_response(lines, "OK|bridge on"))

        print("\n--- Test 4: m1 status (requires F411 connected) ---")
        send(ser, "m1 status")
        lines = read_lines(ser, 2.0)
        has_status = has_response(lines, "STATUS") or has_response(lines, "M1|")
        if has_status:
            report("m1 status -> response", True)
        else:
            report_skip("m1 status -> response", "requires F411 connected")

        print("\n--- Test 5: m1 hall (requires F411 connected) ---")
        send(ser, "m1 hall")
        lines = read_lines(ser, 1.5)
        has_hall = has_response(lines, "Hall") or has_response(lines, "M1|")
        if has_hall:
            report("m1 hall -> response", True)
        else:
            report_skip("m1 hall -> response", "requires F411 connected")

        print("\n--- Test 6: stop (normal stop) ---")
        send(ser, "stop")
        lines = read_lines(ser, 1.0)
        report("stop -> OK", has_response(lines, "OK|normal stop"))

        print("\n--- Test 7: unknown command ---")
        send(ser, "xyzzy")
        lines = read_lines(ser, 1.0)
        report("unknown -> ERR", has_response(lines, "ERR|"))

    print(f"\n{'='*40}")
    print(f"Results: {PASS} passed, {FAIL} failed, {SKIP} skipped")
    print(f"{'='*40}")
    print("\nFor low-duty motor test, use GUI or manually send:")
    print("  m1 mode duty")
    print("  m1 kick off")
    print("  m1 f10")
    print("  stop")

    sys.exit(1 if FAIL > 0 else 0)


if __name__ == "__main__":
    main()
