# Refactor Plan — F411 app_main.c

**Status:** DONE. Modular firmware is now the active `f411-motor-cube/`.
Monolithic original archived in `ref/f411-motor-cube-monolithic/`.

## Problem

`f411-motor-cube/App/Src/app_main.c` is 2218 lines containing the
command parser, application state machine, motion control loop,
watchdog logic, service/gatetest dispatch, and config commands — all
in one file. This makes it hard to reason about, test, and refactor
safely.

## Target modules

| Module | Responsibility extracted from app_main.c |
|--------|----------------------------------------|
| `command_parser` | UART command string parsing and dispatch. All `strcmp`/`strncmp` blocks. |
| `app_state` | Mode (duty/speed), motor phase (stopped/running/brake/neutral/fault), direction, run/stop requests. State transition logic. |
| `motion_control` | Duty-mode kick/ramp state machine. Speed-mode PI integration. `service_motor()` core loop. |
| `safety_watchdog` | Command watchdog (CMD_WATCHDOG_MS), host disconnect (HOST_DISCONNECT_TIMEOUT_MS), fault latch checks. `service_watchdogs()`. |
| `gate_test` | `gatetest` command handler and timeout logic. |
| `service_commands` | `identify`, `scan`, `test` command handlers and service arming. |
| `config_commands` | `pi`, `base`, `boost`, `ramp`, `kick*`, `ramp*`, `defpwm`, `telper`, `debug/dbg` handlers. |
| `map_commands` | `map *` subcommands, `identify` map application, `reload`, `defaults`. |

## Extraction order

Each phase must build and pass before moving to the next.

### Phase 0 — Map boundaries (no code change)

* Read `app_main.c` and identify exact line ranges for each module.
* Document which shared state each module needs.
* Identify circular dependencies.
* Deliverable: this plan updated with line ranges and dependency map.

### Phase 1 — Extract pure parsers/helpers

Start with functions that have **no state dependency** or only read
config:

1. `command_parser` — move command dispatch. The main loop calls
   `CommandParser_Process(line)` instead of inline `if/else` chains.
2. `config_commands` — `pi`, `base`, `boost`, `ramp`, `kick*`, `defpwm`,
   `telper`, `debug/dbg` handlers. These only read/write config state.

### Phase 2 — Extract state machine

3. `app_state` — mode, phase, direction, transition logic. Expose
   `AppState_GetMode()`, `AppState_GetPhase()`, `AppState_SetPhase()`,
   etc.
4. `motion_control` — `service_motor()` body, kick/ramp state machine.
   Depends on `app_state` and `motor_driver`.

### Phase 3 — Extract safety and service

5. `safety_watchdog` — `service_watchdogs()`. Depends on `app_state`
   and `fault_manager`.
6. `gate_test` — gatetest handler. Depends on `motor_driver` and
   `app_state`.
7. `service_commands` — identify/scan/test. Depends on `service_task`
   and `app_state`.
8. `map_commands` — map subcommands. Depends on `bldc_commutation`
   and `storage`.

## Refactor principles

* **Behaviour must not change.** Same commands, same responses, same
  telemetry, same timing.
* Extract functions first, then move them to new files. Do not
  restructure logic and move files in the same step.
* Protocol response strings (`[OK]`, `[ERR]`, `[INFO]`) must be
  preserved exactly.
* Telemetry format and field names must not change.
* Each extraction step: build, verify no behaviour change, commit.
* New modules get their own `.c`/`.h` pair in `App/Src/` and `App/Inc/`.
* Shared state passes through explicit getter/setter functions, not
  extern globals.

## Dependencies to untangle

The main challenge is the `s_app` static struct — it holds all
application state. Options:

1. **Accessor functions** — `AppState_GetTargetDuty()`, etc. Clean but
   verbose.
2. **Opaque pointer** — `app_state.c` owns the struct, exposes accessors.
3. **Partial extraction** — keep `s_app` in `app_main.c` during early
   phases, pass needed fields as function parameters.

Recommendation: start with option 3 (least disruption), evolve toward
option 2 in later phases.

## What stays in app_main.c

After full extraction, `app_main.c` becomes a thin orchestrator:

* `App_Init()` — init calls
* `App_Loop()` — calls to `CommandParser_Process`, `MotionControl_Update`,
  `SafetyWatchdog_Update`, `Telemetry_Update`, etc.
* ISR hooks (`App_Usart2RxIsr`, `App_Tim1BrkIsr`, etc.)
