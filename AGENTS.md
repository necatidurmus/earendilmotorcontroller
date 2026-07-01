# AGENTS.md

Agent rules for the F411 BLDC motor controller project.
Safety-critical power-electronics firmware — read before coding.

## Project map

| Directory | Role | Edit? |
|-----------|------|-------|
| `f411-motor-cube/` | **Active firmware** — STM32Cube/PlatformIO, modular | Yes |
| `f446-bridge-test/` | F446 single-motor UART bridge test | Yes |
| `h7-main/` | H7 upper controller | **No** (unless protocol-compat read) |
| `tools/` | Python GUI/smoke-test | Yes |
| `docs/` | Safety, protocol, bring-up, TIM1 docs | Yes |
| `docs/ai/` | Agent workflow, memory, goals, refactor plan | Yes |
| `ref/` | Archived: legacy Arduino, monolithic firmware, audits | **No** |

**Firmware internals:** `App/` = user motor-control logic. `Core/` =
CubeMX-style generated skeleton (init only). `main.c` only calls
`MX_*_Init`, `App_Init()`, `App_Loop()`.

## Safety rules (non-negotiable)

* Motor outputs **OFF by default** at boot.
* `stop` / `safe` = **coast** (all gates off).
* `brake` = **active brake** (all low-side MOSFETs ON) — only because
  user explicitly requested it. No current sense; use current-limited
  PSU and verify on scope before relying on it.
* No current sense/limit/ADC/INA181. Use a current-limited PSU.
* Never modify TIM1 gate-control hot path without explaining CCxE/CCxNE
  in the change and in `docs/TIM1_GATE_DRIVE.md`.
* Hardware testing: motor **disconnected** first, scope on gate pins.
* Do not re-enable TIM1 break without a physical BKIN pin wired.
* Do not hide problems by disabling watchdogs.
* See `docs/SAFETY.md` and `docs/BRINGUP.md` before any hardware work.

## Architecture rules

* Runtime must be **non-blocking**: no `HAL_Delay`, no blocking UART
  receive, no large stack buffers.
* Hall timestamps use TIM2 (32-bit, 1 MHz). Never TIM1 PWM counter.
* UART: DMA RX circular + DMA TX ring buffer. Do not revert to polling.
* HAL for init; LL/register-direct for the TIM1 hot path.

## Forbidden patterns

* `analogWrite` / `digitalWrite` (Arduino gate control)
* `HAL_Delay` in runtime
* Large stack buffers (128 KB etc.)
* Blocking UART receive / unbounded blocking TX
* `HAL_TIM_Base_MspInit` where `HAL_TIM_PWM_MspInit` is needed
* Shifting channel-1 CCMR mask by another channel's offset

## Protocol compatibility

Commands, telemetry field names, and line format are shared between F411,
F446 bridge, and `tools/f446_motor_gui.py`. Do not silently change them.

* Commands: `mode duty/speed`, `rpm <signed>`, `f/b/f<n>/b<n>`, `stop`,
  `identify`, `clrerr`, `pi`, `base`, `boost`, `ramp`, `hall`, `status`,
  `map`, `gatetest`, `test`, `scan`, `help` — see `docs/PROTOCOL.md`.
* `save`/`savecfg`/`saveall` are **disabled** (flash unsafe).
* Legacy aliases: `mode normal`→`duty`, `mode control`→`speed`.
* Compact telemetry: `RPM:...,T:...,D:...,DIR:...,APP_PH:...,SP:...,
  BRAKE:...,FC:...,H:...,PWM_SET:...,PWM_ACT:...,QDROP:...`
* H7 prefixes lines as `FL|RPM:...` / `FR|...` / `RL|...` / `RR|...`.

## Build

```bash
pio run -d f411-motor-cube          # build firmware
python -m py_compile tools/f446_motor_gui.py  # check Python tools
```

If PlatformIO is unavailable, **report it** — do not guess a substitute.
Do not include `.pio/`, `*.elf`, `*.bin`, `*.o`, `*.map` in commits.

## Agent workflow

1. **Read** — `AGENTS.md`, `docs/ai/MEMORY_BANK.md`, relevant module doc,
   target source file(s) in full.
2. **Plan** — single-line for small fixes; short written plan for
   multi-file changes. Never start a large refactor without alignment.
3. **Apply** — small, focused diffs. One concern per change. Do not mix
   behaviour changes with refactors. Preserve protocol strings and
   telemetry format exactly.
4. **Verify** — build (`pio run -d f411-motor-cube`), `py_compile`, or
   link-check as needed. If tools unavailable, report — do not skip.
5. **Log** — append to `docs/ai/TASK_LOG.md`:

```
## YYYY-MM-DD — Short task title
- **Purpose:** Goal
- **Read:** Key files consulted
- **Changed:** Files modified
- **Why:** Reason
- **Build/test:** Result
- **Remaining risks:** What's unverified?
```

Keep entries short and actionable. No long reports.

## What NOT to do

* Do not add features the user didn't ask for.
* Do not "improve" working code without explicit instruction.
* Do not create files outside the agreed scope.
* Do not commit build artifacts (`.pio/`, `*.elf`, `*.bin`, `*.o`).
* Active brake is enabled by user request; do not remove the
  current-limited-supply / scope-verification warnings from docs.
* Do not enable current sense or hardware break unless explicitly
  requested and safety-reviewed.

## Key docs (read before relevant tasks)

| Doc | When to read |
|-----|-------------|
| `docs/SAFETY.md` | Any hardware or gate-control change |
| `docs/BRINGUP.md` | Hardware testing sequence |
| `docs/TIM1_GATE_DRIVE.md` | Touching `motor_driver.c` or `tim.c` |
| `docs/PROTOCOL.md` | Touching UART parser or telemetry |
| `docs/KNOWN_RISKS.md` | Before assumptions or "it should work" |
| `docs/ai/MEMORY_BANK.md` | Start of any task — current state |
| `ref/REFACTOR_PLAN_F411.md` | Archived — modularization plan (DONE) |
| `ISSUES.md` | Checking known bugs or registering new ones |
| `ROADMAP.md` | Phase plan and stop-the-line criteria |

## Sensitive hot paths

* **TIM1 gate-drive** (`motor_driver.c`): CCxE/CCxNE, sector table,
  allOff, dead-time. Changes here can destroy hardware.
* **Hall sensor** (`hall_sensor.c`): EXTI + TIM2, debounce, edge counter.
  Single-writer architecture — ISR only sets pending flag.
* **UART DMA** (`uart_protocol.c`): DMA RX ring + DMA TX ring. IRQ
  priorities matter.
* **Watchdog / fault handling** (`safety_watchdog.c`, `motion_control.c`):
  CMD_WATCHDOG_MS (800 ms), HOST_DISCONNECT_TIMEOUT_MS (2000 ms).
  Faults stop the motor immediately but are no longer latched; a new
  motion command clears them.
* **F446 service unlock** (`f446-bridge-test/src/main.cpp`): separate
  timer logic — verify independently.
