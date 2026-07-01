# Memory Bank

Project state snapshot for agents. Read this at the start of any task.

## Current state

* **F411 Cube firmware** (`f411-motor-cube/`) is the active production
  firmware. Modular architecture (9 sub-folders, 28 .c / 32 .h files).
  **Hardware-verified** 2026-07-01: motor ran at low RPM with
  current-limited PSU, no excessive current, active brake worked.
  Build: `pio run -d f411-motor-cube`
* **F446 bridge** (`f446-bridge-test/`) is a single-motor UART bridge
  test platform (Arduino/PlatformIO). Active for testing.
* **H7** (`h7-main/`) is out of scope for changes. Read only for
  protocol compatibility understanding.
* **Python tools** (`tools/`): `f446_motor_gui.py` (F446 bridge GUI, active), `f446_serial_smoke_test.py` (smoke test, active).
  Legacy tools (`terminal.py`, `ftdi_h7_*.py`) have been removed.
* **Archived** (`ref/`): legacy Arduino firmware (`ref/f411-motor/`),
  monolithic firmware before modularization
  (`ref/f411-motor-cube-monolithic/`), one-shot audit reports.

## Active decisions

| Decision | Status |
|----------|--------|
| Brake = coast (all gates off) | Active — no current sense |
| Current sense / ADC / INA181 | **None** — by design, this revision |
| UART protocol format | Frozen — F446 bridge/GUI depend on it |
| Telemetry compact format | Frozen — field names and order fixed |
| `save`/`savecfg`/`saveall` | **Enabled** — flash persistence with append-only records |
| HSE = 25 MHz (WeAct BlackPill) | Active — PLLM=25, SYSCLK=96 MHz |
| TIM1 PWM = 20 kHz | Confirmed (ARR=4799, edge-aligned) |
| Hall via EXTI + TIM2 | Confirmed — single-writer architecture |

## Firmware module map

The monolithic 2218-line `app_main.c` has been split into 9 focused
sub-folders. Original archived in `ref/f411-motor-cube-monolithic/`.
See `ref/REFACTOR_PLAN_F411.md` for the original plan.

| Folder | Key files | Responsibility |
|--------|-----------|---------------|
| `app/` | `app_main.c`, `app_state.c`, `app_status.c`, `app_utils.c` | Orchestrator, state singleton, status output, string helpers |
| `command/` | `command_parser.c`, `command_dispatcher.c`, 5 handler .c | UART command dispatch (motion→query→config→service→fault) |
| `motion/` | `motion_control.c`, `motion_safety.c`, `motion_reverse.c`, `safety_watchdog.c` | Motor state machine, safety queries, watchdog |
| `motor/` | `motor_driver.c`, `hall_sensor.c`, `bldc_commutation.c`, `speed_pi.c` | TIM1 gate-drive, Hall capture, 6-step commutation, PI |
| `service/` | `service_task.c`, `service_identify.c`, `service_commutation_test.c`, `gate_test.c` | Identify, scan, test dispatcher |
| `fault/` | `fault_manager.c`, `fault_codes.h` | Fault latching, codes, `clrerr` |
| `telemetry/` | `telemetry.c` | Compact and debug telemetry formatting |
| `storage/` | `storage.c` | Flash load (safe), save (disabled) |
| `protocol/` | `uart_protocol.c` | UART DMA RX/TX, command queue |

## Sensitive areas

These are hot paths or fragile areas — read the linked doc before
touching:

| Area | File(s) | Read first |
|------|---------|-----------|
| TIM1 gate-drive | `motor/motor_driver.c` | `docs/TIM1_GATE_DRIVE.md` |
| Hall/RPM | `motor/hall_sensor.c` | `docs/KNOWN_RISKS.md` §Hall |
| UART DMA | `protocol/uart_protocol.c` | `docs/PROTOCOL.md` |
| Watchdog/fault | `motion/safety_watchdog.c`, `motion/motion_control.c` | `docs/SAFETY.md` |
| F446 service unlock | `f446-bridge-test/src/main.cpp` | check independently |

## Goals

### Completed

* Agent documentation system established (AGENTS.md, CLAUDE.md, docs/ai/).
* F411 `app_main.c` (2218 lines) decomposed into 9 focused modules.
  Build passes, behavior identical. See `docs/ai/TASK_LOG.md`.
* Project root restructured: modular firmware is now `f411-motor-cube/`,
  legacy and monolithic archived in `ref/`.
* **Hardware bring-up completed** (2026-07-01): motor ran at low RPM
  with current-limited PSU. Commands tested: f/b/f<n>/b<n>, stop,
  brake (active brake), rpm, mode duty/speed. No PSU current-limit
  trips, no excessive current, motor rotated smoothly.
* Telemetry `RXB` field documented in AGENTS.md and PROTOCOL.md.
* `storage.c` flash size guard fixed: uses CMSIS `FLASHSIZE_BASE`
  instead of undefined `FLASH_SIZE_DATA_REGISTER`.
* Dead IRQ handler stubs removed from `stm32f4xx_it.c`.

### Short-term (current)

* Higher-RPM testing and PI tuning.
* Hall map `identify` on actual motor (if needed).
* F446 bridge integration testing with hardware.

### Medium-term

* Active braking at higher speeds (current-limited bench supply).
* F446 bridge integration testing with hardware.
* Higher RPM validation and PI gain tuning.

### Long-term

* F411 motor controller is modular, testable, protocol-compatible
  with F446 bridge, and safe for hardware bring-up.
* Reliable Hall-based commutation with validated gate-drive.
* Path toward current sense and active braking (future hardware revision).

## Documentation notes

* `APP_PH` (F411 telemetry) = application phase (STOPPED/RUNNING/etc.)
* `PH` prefix in H7 = `FL|RPM:...` motor position prefix — different
  concept, do not confuse.
* F411 compact format: `RPM:...,T:...,D:...` (no motor prefix)
* H7 relayed format: `FL|RPM:...,T:...,D:...` (motor prefix added by H7)
