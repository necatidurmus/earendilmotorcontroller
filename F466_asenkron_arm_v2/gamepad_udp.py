"""
Mechanical arm gamepad-to-UDP sender.

Sends raw arm payloads to terminal / F446 GUI bridge:

    J,<M1>,<M2>,<M3>,<M4>,<M5>,<M6>
    stop

Default mapping:
    Left stick X      -> M1
    Left stick Y      -> M2
    Right stick X     -> M3
    Right stick Y     -> M4/M5 pitch
    RB/LB             -> M4/M5 twist
    Triangle button   -> M6 positive
    Square button     -> M6 negative

No 55 PWM limit.
Supports --max-pwm 255 or --max-pwm 1024.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from dataclasses import dataclass

try:
    import pygame
except ImportError:
    pygame = None


# ============================================================
# DEFAULT BUTTON MAPPING
# ============================================================
# Common PlayStation / SDL / pygame mapping:
# Cross    = B0
# Circle   = B1
# Square   = B2
# Triangle = B3
#
# If your controller is different, run:
#   python3 gamepad_udp.py --list-controls
#
# Then press Square and Triangle and check which B index becomes 1.

DEFAULT_M6_POS_BUTTON = 3   # Triangle -> M6 positive
DEFAULT_M6_NEG_BUTTON = 2   # Square   -> M6 negative


@dataclass
class Config:
    host: str = "127.0.0.1"
    port: int = 5005
    rate_hz: float = 30.0
    deadzone: float = 0.08
    max_pwm: int = 1024

    # Axis mapping
    left_x_axis: int = 0
    left_y_axis: int = 1
    right_x_axis: int = 3
    right_y_axis: int = 4

    # Y axes usually report up as negative
    invert_left_y: bool = True
    invert_right_y: bool = True

    # Some controllers report full stick as 0.20-0.30 instead of 1.00.
    # 0.25 means raw 0.25 becomes full PWM.
    # Use --axis-full-scale 1.0 if your joystick reaches normal +/-1.0.
    axis_full_scale: float = 0.25

    # Twist buttons
    twist_neg_button: int = 4   # LB / L1
    twist_pos_button: int = 5   # RB / R1

    # M6 control
    m6_mode: str = "buttons"    # disabled | triggers | buttons | axis
    lt_axis: int = 2
    rt_axis: int = 5
    m6_axis: int = 2
    m6_pos_button: int = DEFAULT_M6_POS_BUTTON
    m6_neg_button: int = DEFAULT_M6_NEG_BUTTON

    print_raw: bool = False


def clamp_int(value: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, value))


def clamp_float(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def normalize_axis(value: float, cfg: Config) -> float:
    """
    Convert raw joystick axis to signed normalized value [-1, +1].

    Example:
        raw full stick = 0.25
        axis_full_scale = 0.25
        output = 1.0
    """
    value = clamp_float(value, -1.0, 1.0)

    if abs(value) <= cfg.deadzone:
        return 0.0

    sign = 1.0 if value >= 0 else -1.0

    full = max(cfg.deadzone + 0.01, min(1.0, cfg.axis_full_scale))

    scaled = (abs(value) - cfg.deadzone) / max(1e-6, full - cfg.deadzone)
    scaled = clamp_float(scaled, 0.0, 1.0)

    return sign * scaled


def axis_to_pwm(value: float, cfg: Config) -> int:
    normalized = normalize_axis(value, cfg)
    pwm = int(round(normalized * cfg.max_pwm))
    return clamp_int(pwm, -cfg.max_pwm, cfg.max_pwm)


def get_axis(js, index: int) -> float:
    if index < 0 or index >= js.get_numaxes():
        return 0.0

    try:
        return float(js.get_axis(index))
    except Exception:
        return 0.0


def get_button(js, index: int) -> int:
    if index < 0 or index >= js.get_numbuttons():
        return 0

    try:
        return int(js.get_button(index))
    except Exception:
        return 0


def trigger_axis_to_0_1(raw: float) -> float:
    raw = clamp_float(raw, -1.0, 1.0)

    # Common trigger conventions:
    # released=-1, pressed=+1  -> (raw + 1) / 2
    # released=0,  pressed=+1  -> raw
    if raw < -0.05:
        val = (raw + 1.0) / 2.0
    else:
        val = raw

    if val < 0.08:
        return 0.0

    return clamp_float(val, 0.0, 1.0)


def m6_from_triggers(js, cfg: Config) -> int:
    lt = trigger_axis_to_0_1(get_axis(js, cfg.lt_axis))
    rt = trigger_axis_to_0_1(get_axis(js, cfg.rt_axis))

    signed = rt - lt

    if abs(signed) <= cfg.deadzone:
        return 0

    pwm = int(round(signed * cfg.max_pwm))
    return clamp_int(pwm, -cfg.max_pwm, cfg.max_pwm)


def m6_from_buttons(js, cfg: Config) -> int:
    pos = get_button(js, cfg.m6_pos_button)
    neg = get_button(js, cfg.m6_neg_button)

    # Triangle pressed -> +PWM
    # Square pressed   -> -PWM
    pwm = (pos - neg) * cfg.max_pwm

    return clamp_int(pwm, -cfg.max_pwm, cfg.max_pwm)


def m6_from_axis(js, cfg: Config) -> int:
    return axis_to_pwm(get_axis(js, cfg.m6_axis), cfg)


def build_arm_values(js, cfg: Config) -> tuple[int, int, int, int, int, int]:
    left_x = get_axis(js, cfg.left_x_axis)
    left_y = get_axis(js, cfg.left_y_axis)
    right_x = get_axis(js, cfg.right_x_axis)
    right_y = get_axis(js, cfg.right_y_axis)

    if cfg.invert_left_y:
        left_y = -left_y

    if cfg.invert_right_y:
        right_y = -right_y

    # M1, M2, M3
    m1 = axis_to_pwm(left_x, cfg)
    m2 = axis_to_pwm(left_y, cfg)
    m3 = axis_to_pwm(right_x, cfg)

    # M4/M5 pitch from right stick Y
    pitch = axis_to_pwm(right_y, cfg)

    # M4/M5 twist from LB/RB
    twist_button = get_button(js, cfg.twist_pos_button) - get_button(js, cfg.twist_neg_button)
    twist = clamp_int(twist_button * cfg.max_pwm, -cfg.max_pwm, cfg.max_pwm)

    m4 = clamp_int(pitch + twist, -cfg.max_pwm, cfg.max_pwm)
    m5 = clamp_int(pitch - twist, -cfg.max_pwm, cfg.max_pwm)

    # M6
    if cfg.m6_mode == "disabled":
        m6 = 0
    elif cfg.m6_mode == "triggers":
        m6 = m6_from_triggers(js, cfg)
    elif cfg.m6_mode == "buttons":
        m6 = m6_from_buttons(js, cfg)
    elif cfg.m6_mode == "axis":
        m6 = m6_from_axis(js, cfg)
    else:
        m6 = 0

    return m1, m2, m3, m4, m5, m6


def build_arm_packet(js, cfg: Config) -> str:
    m1, m2, m3, m4, m5, m6 = build_arm_values(js, cfg)
    return f"J,{m1},{m2},{m3},{m4},{m5},{m6}"


def send_udp(sock: socket.socket, address: tuple[str, int], text: str) -> None:
    sock.sendto(text.encode("utf-8"), address)


def print_controls(js) -> None:
    pygame.event.pump()

    print(f"Gamepad: {js.get_name()}")
    print(f"Axes: {js.get_numaxes()}, Buttons: {js.get_numbuttons()}, Hats: {js.get_numhats()}")
    print("Move each stick/button and watch the values below. Ctrl+C to exit.")

    try:
        while True:
            pygame.event.pump()

            axes = ", ".join(
                f"A{i}:{get_axis(js, i):+.3f}"
                for i in range(js.get_numaxes())
            )

            buttons = ", ".join(
                f"B{i}:{get_button(js, i)}"
                for i in range(js.get_numbuttons())
            )

            print(f"\r{axes} | {buttons}   ", end="", flush=True)
            time.sleep(0.05)

    except KeyboardInterrupt:
        print()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Send mechanical arm gamepad packets to terminal/F446 GUI over UDP."
    )

    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="UDP target host. Use the PC/RPi running the F446 GUI bridge."
    )

    parser.add_argument(
        "--port",
        type=int,
        default=5005,
        help="UDP target port. Default: 5005"
    )

    parser.add_argument(
        "--rate",
        type=float,
        default=30.0,
        help="Send rate in Hz. Default: 30"
    )

    parser.add_argument(
        "--deadzone",
        type=float,
        default=0.08,
        help="Joystick deadzone 0..1. Default: 0.08"
    )

    parser.add_argument(
        "--max-pwm",
        type=int,
        default=1024,
        help="Maximum absolute PWM value. Use 255 or 1024. Default: 1024"
    )

    parser.add_argument(
        "--axis-full-scale",
        type=float,
        default=0.25,
        help="Raw axis magnitude that maps to full PWM. Default: 0.25"
    )

    parser.add_argument("--left-x-axis", type=int, default=0)
    parser.add_argument("--left-y-axis", type=int, default=1)
    parser.add_argument("--right-x-axis", type=int, default=3)
    parser.add_argument("--right-y-axis", type=int, default=4)

    parser.add_argument("--no-invert-left-y", action="store_true")
    parser.add_argument("--no-invert-right-y", action="store_true")

    parser.add_argument(
        "--twist-neg-button",
        type=int,
        default=4,
        help="LB/L1 default=4"
    )

    parser.add_argument(
        "--twist-pos-button",
        type=int,
        default=5,
        help="RB/R1 default=5"
    )

    parser.add_argument(
        "--m6-mode",
        choices=["disabled", "triggers", "buttons", "axis"],
        default="buttons",
        help="M6 mode. Default: buttons"
    )

    parser.add_argument("--lt-axis", type=int, default=2)
    parser.add_argument("--rt-axis", type=int, default=5)
    parser.add_argument("--m6-axis", type=int, default=2)

    parser.add_argument(
        "--m6-pos-button",
        type=int,
        default=DEFAULT_M6_POS_BUTTON,
        help="M6 positive button. Default: Triangle B3"
    )

    parser.add_argument(
        "--m6-neg-button",
        type=int,
        default=DEFAULT_M6_NEG_BUTTON,
        help="M6 negative button. Default: Square B2"
    )

    parser.add_argument(
        "--print-raw",
        action="store_true",
        help="Print raw axis values and packet."
    )

    parser.add_argument(
        "--list-controls",
        action="store_true",
        help="Continuously print raw axes/buttons and exit without sending UDP."
    )

    args = parser.parse_args()

    cfg = Config(
        host=args.host,
        port=args.port,
        rate_hz=max(1.0, args.rate),
        deadzone=max(0.0, min(0.8, args.deadzone)),

        # Important:
        # No 255 hard clamp.
        # Allows 255 / 1024 tests.
        max_pwm=max(0, min(1024, args.max_pwm)),

        axis_full_scale=max(0.05, min(1.0, args.axis_full_scale)),

        left_x_axis=args.left_x_axis,
        left_y_axis=args.left_y_axis,
        right_x_axis=args.right_x_axis,
        right_y_axis=args.right_y_axis,

        invert_left_y=not args.no_invert_left_y,
        invert_right_y=not args.no_invert_right_y,

        twist_neg_button=args.twist_neg_button,
        twist_pos_button=args.twist_pos_button,

        m6_mode=args.m6_mode,
        lt_axis=args.lt_axis,
        rt_axis=args.rt_axis,
        m6_axis=args.m6_axis,
        m6_pos_button=args.m6_pos_button,
        m6_neg_button=args.m6_neg_button,

        print_raw=args.print_raw,
    )

    if pygame is None:
        print("pygame is not installed. Install it with:", file=sys.stderr)
        print("  pip install pygame", file=sys.stderr)
        return 1

    pygame.init()
    pygame.joystick.init()

    if pygame.joystick.get_count() <= 0:
        print("No gamepad/joystick found. Connect it and run again.", file=sys.stderr)
        return 1

    js = pygame.joystick.Joystick(0)
    js.init()

    if args.list_controls:
        print_controls(js)
        pygame.quit()
        return 0

    address = (cfg.host, cfg.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    period = 1.0 / cfg.rate_hz

    print(f"Gamepad: {js.get_name()}")
    print(f"Sending UDP arm packets to {cfg.host}:{cfg.port}")
    print("Format: J,M1,M2,M3,M4,M5,M6")
    print(f"max_pwm={cfg.max_pwm}")
    print(f"axis_full_scale={cfg.axis_full_scale}")
    print(f"deadzone={cfg.deadzone}")
    print(f"m6_mode={cfg.m6_mode}")
    print(f"M6 positive button index: {cfg.m6_pos_button}")
    print(f"M6 negative button index: {cfg.m6_neg_button}")
    print("Default M6: Triangle = +PWM, Square = -PWM")
    print("No 55 PWM limit in this sender.")
    print("Press Ctrl+C to send stop and exit.")

    last_print = 0.0

    try:
        while True:
            pygame.event.pump()

            packet = build_arm_packet(js, cfg)
            send_udp(sock, address, packet)

            now = time.time()

            if now - last_print >= 0.2:
                last_print = now

                if cfg.print_raw:
                    raw = (
                        f"LX={get_axis(js, cfg.left_x_axis):+.3f} "
                        f"LY={get_axis(js, cfg.left_y_axis):+.3f} "
                        f"RX={get_axis(js, cfg.right_x_axis):+.3f} "
                        f"RY={get_axis(js, cfg.right_y_axis):+.3f} "
                        f"B{cfg.m6_pos_button}={get_button(js, cfg.m6_pos_button)} "
                        f"B{cfg.m6_neg_button}={get_button(js, cfg.m6_neg_button)}"
                    )
                    print(f"{packet} | {raw}")
                else:
                    print(packet)

            time.sleep(period)

    except KeyboardInterrupt:
        print("\nStopping arm...")

        for _ in range(5):
            send_udp(sock, address, "stop")
            time.sleep(0.05)

    finally:
        try:
            sock.close()
        except Exception:
            pass

        pygame.quit()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
