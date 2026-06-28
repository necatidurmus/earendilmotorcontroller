# f411-motor-cube

STM32F411 BLDC motor controller firmware — STM32Cube / PlatformIO.
Modular architecture, behavior-compatible with the original monolithic
firmware (archived in `ref/f411-motor-cube-monolithic/`).

## Build

```bash
pio run -d f411-motor-cube
```

## Folder structure

```
App/
├── Inc/
│   ├── app/          app_main, app_state, app_config, app_types, app_status, app_utils
│   ├── command/      command_parser, command_types, dispatcher, 5 handler headers
│   ├── motion/       motion_control, motion_safety, motion_reverse, safety_watchdog
│   ├── motor/        motor_driver, bldc_commutation, hall_sensor, speed_pi
│   ├── service/      service_task, service_identify, service_commutation_test, gate_test
│   ├── fault/        fault_manager, fault_codes
│   ├── telemetry/    telemetry
│   ├── storage/      storage
│   └── protocol/     uart_protocol
└── Src/
    ├── app/          app_main, app_state, app_status, app_utils
    ├── command/      command_parser, command_dispatcher, 5 handler .c files
    ├── motion/       motion_control, motion_safety, motion_reverse, safety_watchdog
    ├── motor/        motor_driver, bldc_commutation, hall_sensor, speed_pi
    ├── service/      service_task, service_identify, service_commutation_test, gate_test
    ├── fault/        fault_manager
    ├── telemetry/    telemetry
    ├── storage/      storage
    └── protocol/     uart_protocol
```

## Module map

| Module | Files | Responsibility |
|--------|-------|---------------|
| **app/** | `app_main.c`, `app_state.c`, `app_status.c`, `app_utils.c` | Orchestrator, AppState singleton, status/help output, string helpers |
| **command/** | `command_parser.c`, `command_dispatcher.c`, `command_handlers_*.c` | Trim/lower, dispatch to 5 category handlers (motion, query, config, service, fault) |
| **motion/** | `motion_control.c`, `motion_safety.c`, `motion_reverse.c`, `safety_watchdog.c` | Motor state machine, stop/run/neutral, kick/ramp, safety queries, watchdog |
| **motor/** | `motor_driver.c`, `bldc_commutation.c`, `hall_sensor.c`, `speed_pi.c` | TIM1 gate-drive, 6-step commutation, EXTI+TIM2 Hall, speed PI |
| **service/** | `service_task.c`, `service_identify.c`, `service_commutation_test.c`, `gate_test.c` | Service dispatcher, identify algorithm, scan/test, gate test timeout |
| **fault/** | `fault_manager.c`, `fault_codes.h` | Fault latching, codes, `clrerr` |
| **telemetry/** | `telemetry.c` | Compact and debug telemetry formatting |
| **storage/** | `storage.c` | Flash load (safe), save (disabled) |
| **protocol/** | `uart_protocol.c` | UART DMA RX/TX, command queue, line parsing |

## Architecture

- `App/` = user motor-control logic (modular, 9 sub-folders)
- `Core/` = CubeMX-style generated skeleton (init only)
- `main.c` only calls `MX_*_Init`, `App_Init()`, `App_Loop()`
- `tools/` is at project root (not duplicated here)

## Safety

- No active brake (coast-only, no current sense)
- No blocking `HAL_Delay`
- No Arduino functions (`analogWrite`, `digitalWrite`)
- Motor outputs OFF by default at boot
- Fault latching requires `clrerr`
