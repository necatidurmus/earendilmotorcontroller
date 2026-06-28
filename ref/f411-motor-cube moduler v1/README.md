# f411-motor-cube

STM32F411 BLDC motor controller firmware — STM32Cube / PlatformIO.
Modular architecture, behavior-compatible with the original monolithic
firmware (archived in `ref/f411-motor-cube-monolithic/`).

## Build

```bash
pio run -d f411-motor-cube
```

## Module map

| Module | Files | Lines | Responsibility |
|--------|-------|-------|---------------|
| **app_types** | `app_types.h` | 30 | Shared enums: `AppMode`, `MotorPhase`, `Direction` |
| **app_state** | `app_state.h/.c` | 139 | `AppState` singleton, `AppState_Get()`, `AppState_InitDefaults()` |
| **app_utils** | `app_utils.h/.c` | 84 | String helpers: trim, lower, starts_with, parse_long/float_after |
| **app_status** | `app_status.h/.c` | 178 | `print_status`, `print_help`, `print_hall_map` |
| **command_parser** | `command_parser.h/.c` | 1087 | Full UART command dispatch (extracted from `handle_command`) |
| **motion_control** | `motion_control.h/.c` | 464 | Motor state machine, stop/run/neutral, kick/ramp, Hall freshness |
| **safety_watchdog** | `safety_watchdog.h/.c` | 48 | Command watchdog (800ms) + host disconnect (2000ms) |
| **gate_test** | `gate_test.h/.c` | 42 | Gate test timeout logic |
| **app_main** | `app_main.h/.c` | 218 | Thin orchestrator: `App_Init`, `App_Loop`, ISR shims, accessors |

## Architecture

- `App/` = user motor-control logic (modular)
- `Core/` = CubeMX-style generated skeleton (init only)
- `main.c` only calls `MX_*_Init`, `App_Init()`, `App_Loop()`
- `tools/` is at project root (not duplicated here)

## Safety

- No active brake (coast-only, no current sense)
- No blocking `HAL_Delay`
- No Arduino functions (`analogWrite`, `digitalWrite`)
- Motor outputs OFF by default at boot
- Fault latching requires `clrerr`
