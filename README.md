# BLDC Motor Controller Project

## Overview

STM32F411 Hall-sensor BLDC hub motor controller with F446 UART bridge
and PC GUI.

**Active system flow:** PC GUI (`f446_motor_gui.py`) → F446 bridge → F411 motor controller

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
├── h7-main/             # INACTIVE: H7 upper controller (kept for reference only)
├── tools/               # Python tools (GUI, smoke tests)
│   ├── f446_motor_gui.py        # ACTIVE: F446 bridge GUI
│   ├── f446_serial_smoke_test.py # ACTIVE: F446 bridge serial smoke test
│   └── requirements.txt          # Python dependencies (pyserial)
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
* 8-band base/boost PI tuning (4000 PWM range)
* Hall map identify / save / load with flash persistence
* Fault handling — faults are non-latching; new motion command clears them
* Duty-ramp with configurable step and interval (`ramprate`, `rampms`)
* Kick start with configurable duty and duration (`kickduty`, `kickms`)
* Service/gate test commands gated by F446 bridge service lock

### Pin Map
| Pin | Function |
|-----|----------|
| PA8, PA7 | Phase A high/low (TIM1_CH1/CH1N) |
| PA9, PB0 | Phase B high/low (TIM1_CH2/CH2N) |
| PA10, PB1 | Phase C high/low (TIM1_CH3/CH3N) |
| PB6, PB7, PB8 | Hall sensors (EXTI, pull-up) |
| PA2, PA3 | UART TX/RX (115200 baud) |
| PC13 | LED |

## Active Firmware: f446-bridge-test

F446 UART bridge between PC and F411 motor controller.

```bash
cd f446-bridge-test
pio run                    # build
pio run -t upload          # flash via ST-Link
```

### Bridge Commands
```
ping                       -> pong
help                       -> bridge help
bridge on/off              -> enable/disable telemetry forwarding
bridge unlock_service ...  -> unlock dangerous service commands (30s)
bridge lock_service        -> re-lock
bridge status              -> show bridge state
m1 <cmd>                   -> send <cmd> to F411 (with safety filter)
stop                       -> normal stop: rpm 0 + stop
safe / alloff              -> coast stop (no fault latch)
estop                      -> emergency all-off
```

## Legacy & Archived (`ref/`)

* `ref/f411-motor/` — Original Arduino firmware (reference only)
* `ref/f411-motor-cube-monolithic/` — Monolithic firmware before modularization

**Do not build or modify archived firmware.**

## Inactive / Removed Targets

* **H7** (`h7-main/`) — Not an active target in this repo. Kept for reference only; do not modify.
* **`ftdi_h7_gui.py`** — Removed. Was FTDI direct-control GUI for H7 workflow.
* **`ftdi_h7_emulator.py`** / **`ftdi_h7_client.py`** — Removed. CLI test tools for H7 workflow.
* **`terminal.py`** — Removed. Legacy H7 terminal GUI; replaced by `f446_motor_gui.py`.
* **H7 F446 import plan docs** — Removed. H7 integration plans are no longer active.

## Python Tools

```bash
python3 tools/f446_motor_gui.py              # F446 bridge GUI (active)
python3 tools/f446_serial_smoke_test.py       # F446 bridge serial smoke test
```

## Current Sense

This hardware revision does not use current sense. Protection is
provided through Hall timeout, fault state, watchdog, and manual stop
behaviors. Use a current-limited bench supply when operating the motor.
Do not enable current sense or hardware break unless explicitly
requested and safety-reviewed.

## Safety Rules

* Motor outputs are **OFF by default** at boot
* **Active brake** (`brake`/`x`) — all low-side MOSFETs ON; use only with current-limited bench supply
* **No current sense** — use a current-limited PSU
* **Motor disconnected** for first TIM1 scope test
* **Non-latching faults** — a new motion command clears the fault and restarts
* **Command heartbeat** — duty and speed modes stop if host disconnects
* **TIM1 all-off** depends on gate driver input pulldown resistors for physical LOW;
  verify with oscilloscope at boot/stop/fault
* **Mode change** requires motor stopped (`stop` first, then `mode duty`/`mode speed`)
* **Service/gate test** blocks motion commands until complete
* **No hardware break** — do not enable TIM1 break without a physical BKIN pin wired
* Verify all gate inputs with oscilloscope before connecting motor
* **Storage** — `savecfg` saves runtime config to flash; `loadcfg` loads from flash.
  Invalid data falls back to safe defaults.

### Stop vs Emergency Stop

| Command | Behavior | Fault latch | Restart |
|---------|----------|-------------|---------|
| `stop` / `s` | Normal safe stop, immediate all-off | No | `f`/`b`/`rpm` |
| `brake` / `x` | Active brake (all low-side ON); coast in bring-up | No | `f`/`b`/`rpm` |
| `estop` | Emergency stop with fault | **Yes** | `clrerr` first |
| `safe` / `alloff` | Coast stop (no fault) | No | `f`/`b`/`rpm` |

* `stop` cancels any active service task or gate test immediately.
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
