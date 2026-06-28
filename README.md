# BLDC Motor Controller Project

## Overview

STM32F411 Hall-sensor BLDC hub motor controller with H7 upper controller
and PC terminal GUI.

**Active firmware: `f411-motor-cube/`** — STM32Cube / PlatformIO modular
rewrite. Legacy firmware and monolithic original archived in `ref/`.

> **Not hardware-tested.** See `docs/BRINGUP.md` for the staged
> bring-up sequence. Do not connect a motor until TIM1 outputs are
> scope-verified.

## Structure

```
bldc-project-stm/
├── f411-motor-cube/     # ACTIVE: Modular STM32Cube/PlatformIO F411 firmware
│   ├── platformio.ini   # Build config (framework=stm32cube)
│   ├── f411-motor-cube.ioc  # CubeMX peripheral config
│   ├── Core/            # CubeMX-style generated skeleton
│   └── App/             # Modular motor-control application (8 modules)
├── f446-bridge-test/    # ACTIVE: F446 UART bridge firmware (single-motor test)
├── h7-main/             # H7 upper controller firmware (reference only)
├── tools/               # Python tools (terminal.py, GUI, FTDI, smoke tests)
├── docs/                # Safety, bring-up, protocol, TIM1 docs
├── ref/                 # Archived: legacy Arduino, monolithic firmware, audits
├── AGENTS.md            # Agent rules
├── ISSUES.md            # Issue register
├── ROADMAP.md           # Phase plan
└── README.md            # This file
```

## Active Firmware: f411-motor-cube

### Build & Flash
```bash
cd f411-motor-cube
pio run                    # build
pio run -t upload          # flash via ST-Link
pio device monitor         # serial monitor (115200 baud)
```

### Key Features
* TIM1 edge-aligned 20 kHz PWM, 6-step commutation
* EXTI Hall capture on PB6/PB7/PB8 with TIM2 1 MHz timestamps
* DMA RX circular + DMA TX ring buffer UART
* Speed PI mode with command heartbeat safety
* Fault latching — requires `clrerr` before new motion
* Conservative defaults (Kp=0.8, Ki=0.05, max PWM=180, duty 0..250)

### Pin Map
| Pin | Function |
|-----|----------|
| PA8, PA7 | Phase A high/low (TIM1_CH1/CH1N) |
| PA9, PB0 | Phase B high/low (TIM1_CH2/CH2N) |
| PA10, PB1 | Phase C high/low (TIM1_CH3/CH3N) |
| PB6, PB7, PB8 | Hall sensors (EXTI, pull-up) |
| PA2, PA3 | UART TX/RX (115200 baud) |
| PC13 | LED |

## Legacy & Archived (`ref/`)

* `ref/f411-motor/` — Original Arduino firmware (reference only)
* `ref/f411-motor-cube-monolithic/` — Monolithic firmware before modularization

**Do not build or modify archived firmware.**

## H7 Upper Controller

```bash
cd h7-main
pio run
pio run -t upload
```

Sends `rpm <signed>`, `f<pwm>`, `b<pwm>`, `stop`, `identify` to F411.
Prefixes telemetry as `FL|RPM:...` for `tools/terminal.py`.

## Python Tools

```bash
python3 tools/terminal.py              # H7 terminal GUI
python3 tools/ftdi_h7_gui.py           # F411 direct GUI
python3 tools/ftdi_h7_emulator.py      # F411 CLI test
```

## Documentation

* `docs/SAFETY.md` — Power-stage safety, shoot-through, stop-the-line
* `docs/BRINGUP.md` — Staged hardware bring-up sequence
* `docs/TIM1_GATE_DRIVE.md` — CCxE/CCxNE model, sector truth table
* `docs/PROTOCOL.md` — UART command/telemetry reference
* `docs/KNOWN_RISKS.md` — Unverified assumptions
* `ISSUES.md` — Issue register
* `ROADMAP.md` — Phase plan

## Safety Rules

* Motor outputs are **OFF by default** at boot
* **No active brake** during first bring-up
* **No current sense** — use a current-limited PSU
* **Motor disconnected** for first TIM1 scope test
* **Fault latching** — `clrerr` required before new motion; motor does NOT auto-restart
* **Command heartbeat** — both duty and speed modes stop if H7/terminal disconnects
* **TIM1 all-off** depends on gate driver input pulldown resistors for physical LOW;
  verify with oscilloscope at boot/stop/fault
* **Mode change** requires motor stopped (`stop` first, then `mode duty`/`mode speed`)
* **Service/gate test** blocks motion commands until complete
* **No hardware break** — do not enable TIM1 break without a physical BKIN pin wired
* Verify all gate inputs with oscilloscope before connecting motor
* **Storage** — persistent save is **disabled** (`[ERR] Persistent storage
  disabled in this build`); boot-time load from flash is enabled with
  full validation (magic, version, checksum, range check).  Invalid
  data falls back to safe defaults.

### Stop vs Emergency Stop

| Command | Behavior | Fault latch | Restart |
|---------|----------|-------------|---------|
| `stop` / `s` | Normal safe stop, immediate all-off | No | `f`/`b`/`rpm` |
| `brake` / `x` | Coast stop (same as stop in bring-up) | No | `f`/`b`/`rpm` |
| `estop` | Emergency stop with fault latch | **Yes** | `clrerr` first |
| `safe` / `alloff` | Same as stop (no fault) | No | `f`/`b`/`rpm` |

* `stop` cancels any active service task or gate test immediately.
* `brake` cancels service tasks and gate test, then coasts (no active brake in bring-up).
* Gate test / service task during `stop` or `brake` → outputs go off immediately.

### Hall Map Rules

* Raw 0 (0b000) and raw 7 (0b111) are **always invalid** — must map to 255.
* `identify` rejects unstable Hall readings (hallA ≠ hallB).
* `identify` rejects raw 0 or 7 readings.
* Flash-loaded maps are validated with the same rules.

### Telemetry Fields

* `APP_PH` — application phase (STOPPED/RUNNING/BRAKE/NEUTRAL/FAULT)
* `SPD_PH` — speed PI phase
* `FC` — fault code
* `QDROP` — command queue overflow drop count
