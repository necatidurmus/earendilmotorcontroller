# Architecture Index

Quick file map for agents. Find the right file fast.

## F411 active firmware (`f411-motor-cube/`)

Modular architecture, 9 sub-folders under `App/`. Build: `pio run -d f411-motor-cube`

### app/

| File | Role |
|------|------|
| `App/Src/app/app_main.c` | **Thin orchestrator** — App_Init, App_Loop, ISR shims, accessors |
| `App/Src/app/app_state.c` | AppState singleton, `AppState_Get()` |
| `App/Src/app/app_status.c` | Status/help/hall-map text output |
| `App/Src/app/app_utils.c` | String helpers: trim, lower, parse_long/float_after |
| `App/Inc/app/app_config.h` | All tunable defaults, pin map, protocol constants |
| `App/Inc/app/app_types.h` | Shared enums: AppMode, MotorPhase, Direction |
| `App/Inc/app/app_state.h` | AppState struct typedef, accessor declarations |

### command/

| File | Role |
|------|------|
| `App/Src/command/command_parser.c` | Trim, lowercase, call dispatcher |
| `App/Src/command/command_dispatcher.c` | Route to 5 category handlers (motion→query→config→service→fault) |
| `App/Src/command/command_handlers_motion.c` | `f`, `b`, `stop`, `estop`, `safe`, `alloff`, `pwm`, `mode`, `rpm` |
| `App/Src/command/command_handlers_query.c` | `status`, `help`, `hall`, `spstat`, `debug`, `telper` |
| `App/Src/command/command_handlers_config.c` | `pi`, `kp`, `ki`, `base`, `boost`, `ramp`, `map`, `kick*`, `ramp*`, `defpwm`, `defaults`, `loadcfg`, `save*` |
| `App/Src/command/command_handlers_service.c` | `arm`, `disarm`, `identify`, `scan`, `test`, `gatetest` |
| `App/Src/command/command_handlers_fault.c` | `clrerr` |
| `App/Inc/command/command_types.h` | `CommandCategory` enum |

### motion/

| File | Role |
|------|------|
| `App/Src/motion/motion_control.c` | Motor state machine, stop/run/neutral, kick/ramp |
| `App/Src/motion/motion_safety.c` | `MotionControl_Allowed()`, `MotionControl_ServiceBusy()` |
| `App/Src/motion/motion_reverse.c` | `MotionControl_BeginNeutralSwitch()` |
| `App/Src/motion/safety_watchdog.c` | Command watchdog (800ms) + host disconnect (2000ms) |

### motor/

| File | Role |
|------|------|
| `App/Src/motor/motor_driver.c` | TIM1 gate-drive hot path — CCxE/CCxNE, sector table, allOff, ApplyStep |
| `App/Src/motor/hall_sensor.c` | EXTI + TIM2 Hall capture, debounce, edge counter, RPM. Single-writer |
| `App/Src/motor/bldc_commutation.c` | 6-step commutation helpers |
| `App/Src/motor/speed_pi.c` | Speed PI controller — feed-forward base, boost, ramp |

### service/

| File | Role |
|------|------|
| `App/Src/service/service_task.c` | Service dispatcher: Init, Request, Cancel, IsActive, Update |
| `App/Src/service/service_identify.c` | Identify algorithm: toggle sectors, read Hall, build map |
| `App/Src/service/service_commutation_test.c` | Scan (monitor Hall 10s) and test (apply 6 sectors) |
| `App/Src/service/gate_test.c` | Gate test timeout |

### fault/

| File | Role |
|------|------|
| `App/Src/fault/fault_manager.c` | Fault latching, `clrerr`, gate-off on fault |
| `App/Inc/fault/fault_codes.h` | FaultCode enum (shared across modules) |

### telemetry/ storage/ protocol/

| File | Role |
|------|------|
| `App/Src/telemetry/telemetry.c` | Compact and debug telemetry formatting |
| `App/Src/storage/storage.c` | Flash load (safe), save (disabled) |
| `App/Src/protocol/uart_protocol.c` | UART DMA RX/TX, command queue, line parsing |

### Core/

| File | Role |
|------|------|
| `Core/Src/tim.c` | TIM1/TIM2/TIM4 init (CubeMX-style). Hand-edited |
| `Core/Src/usart.c` | USART2 init |
| `Core/Src/gpio.c` | GPIO config (Hall EXTI, LED) |
| `Core/Src/main.c` | Calls MX_*_Init, App_Init(), App_Loop(). No app logic |
| `platformio.ini` | Build config (framework=stm32cube) |
| `f411-motor-cube.ioc` | CubeMX peripheral config. Hand-maintained |

## F446 bridge (`f446-bridge-test/`)

| File | Role |
|------|------|
| `src/main.cpp` | Single-motor UART bridge. PC↔F411 relay. Service unlock timer |
| `include/f446_bridge_config.h` | Pin config, buffer sizes, timing |

## Python tools (`tools/`)

| File | Role |
|------|------|
| `terminal.py` | H7 terminal GUI — telemetry table, command send |
| `f446_motor_gui.py` | F446 direct motor GUI |
| `f446_serial_smoke_test.py` | F446 serial smoke test |
| `ftdi_h7_gui.py` | FTDI direct-control GUI |
| `ftdi_h7_client.py` | FTDI H7 client library |
| `ftdi_h7_emulator.py` | FTDI H7 CLI test |

## H7 upper controller (`h7-main/`) — read only

| File | Role |
|------|------|
| `src/main.cpp` | Wheelbridge telemetry relay, motor command dispatch |
| `src/motor_dispatcher.cpp` | Sends `rpm`, `f`, `b`, `stop`, `identify` to F411 |

## Docs

| File | Covers |
|------|--------|
| `docs/SAFETY.md` | Power-stage safety, shoot-through, stop-the-line |
| `docs/BRINGUP.md` | Staged hardware bring-up sequence |
| `docs/TIM1_GATE_DRIVE.md` | CCxE/CCxNE model, sector truth table, dead-time |
| `docs/PROTOCOL.md` | Full UART command/telemetry reference |
| `docs/KNOWN_RISKS.md` | Unverified assumptions and risks |
| `docs/HALL_IDENTIFY.md` | Hall identify algorithm |
| `docs/REVIEW_REPORT.md` | One-shot code review (archived) |
| `docs/ai/` | Agent workflow, memory bank, task log |

## Archived (`ref/`) — do not modify

| Directory | Content |
|-----------|---------|
| `ref/f411-motor/` | Legacy Arduino firmware (reference only) |
| `ref/f411-motor-cube-monolithic/` | Monolithic firmware before modularization |
| `ref/BUG_REPORT.md` | One-shot bug report |
| `ref/COMPREHENSIVE_AUDIT.md` | One-shot audit report |
