# f411-motor-cube

STM32Cube / PlatformIO rewrite of the F411 Hall-sensor BLDC hub motor
controller.

The original Arduino-framework firmware is preserved untouched in
`../f411-motor/` and is used only as a behavioural/protocol reference.
This project lives in a separate folder so the two firmwares never
cross-build, and so the STM32CubeMX `.ioc` and the user application
can be edited independently of the legacy code.

> **Not hardware-tested.** Scope verification of the TIM1 gate
> outputs is required before any motor is connected. See
> `docs/BRINGUP.md` and `docs/SAFETY.md`.

## Build

```
pio run -d f411-motor-cube
```

If PlatformIO is unavailable on your machine, report it — do not
guess a substitute build command. Build artifacts (`.pio/`, `*.elf`,
`*.bin`, `*.o`, `*.map`) are gitignored and must never be committed
or zipped.

`pio run -t upload` flashes via ST-Link. `pio device monitor` opens
the command UART at 115200 baud.

## Layout

```
f411-motor-cube/
├── platformio.ini
├── f411-motor-cube.ioc           (open in STM32CubeMX; hand-edited)
├── Core/                         CubeMX-style generated skeleton
│   ├── Inc/                      main.h, tim.h, usart.h, gpio.h, ...
│   └── Src/                      main.c (MX init + App_Init/Loop),
│                                  tim.c, usart.c, gpio.c, stm32f4xx_it.c
├── Drivers/                      (empty — pulled from framework-stm32cube)
└── App/                          user motor-control application
    ├── Inc/                      app_config.h, motor_driver.h, ...
    └── Src/                      app_main.c, motor_driver.c, hall_sensor.c,
                                  speed_pi.c, telemetry.c, uart_protocol.c,
                                  fault_manager.c, bldc_commutation.c,
                                  service_task.c, storage.c
```

`Core/` is generated-style code. `App/` is user code. `main.c` only
calls `MX_*_Init`, `App_Init()` and `App_Loop()`. HAL is used for
init; the TIM1 gate hot path in `motor_driver.c` writes registers
directly. Runtime is non-blocking (no `HAL_Delay`, no blocking
`while` in the motor path).

## Pin map (matches the legacy Arduino firmware)

| Function | Pin  | AF / Notes              |
|----------|------|-------------------------|
| AH       | PA8  | TIM1_CH1   (CC1E)       |
| BH       | PA9  | TIM1_CH2   (CC2E)       |
| CH       | PA10 | TIM1_CH3   (CC3E)       |
| AL       | PA7  | TIM1_CH1N  (CC1NE)      |
| BL       | PB0  | TIM1_CH2N  (CC2NE)      |
| CL       | PB1  | TIM1_CH3N  (CC3NE)      |
| HALL_A   | PB6  | input, pull-up (bit 2)  |
| HALL_B   | PB7  | input, pull-up (bit 1)  |
| HALL_C   | PB8  | input, pull-up (bit 0)  |
| USART2 TX| PA2  | AF7                     |
| USART2 RX| PA3  | AF7, DMA1 Stream5 ch.4  |
| LED      | PC13 | output (active low)     |

The AH/BH/CH and AL/BL/CL pin assignment is copied verbatim from the
validated Arduino firmware (`f411-motor/src/main.cpp`).

## Clock tree

* HSE = 8 MHz crystal
* PLL: M=8, N=192, P=2, Q=4
* SYSCLK = 96 MHz
* APB1 = 48 MHz  (TIM2/TIM4 timer clock = 96 MHz)
* APB2 = 96 MHz  (TIM1 timer clock = 96 MHz)

## PWM (TIM1)

* Mode: **edge-aligned, upcounting** (`TIM_COUNTERMODE_UP`).
* ARR = 4799, prescaler 0 → **f_pwm = 96 MHz / 4800 = 20.0 kHz**.
* `PWM_PERIOD_TICKS` in `app_config.h` = 4799 (kept in sync with ARR).
* Dead-time DTG = 63 (~0.66 µs @ 96 MHz) — **not bench-verified**.
  The TIM1 clock is unchanged (96 MHz) so DTG=63 gives the same
  dead-time as before the 20 kHz PWM change.
* Asynchronous six-step: one phase high-side PWM, one *different*
  phase low-side static ON, third phase floating. Synchronous
  complementary PWM is never enabled. See `docs/TIM1_GATE_DRIVE.md`.

> Center-aligned mode would give 10 kHz for the same ARR
> (`f = TIM_CLK / (2*(ARR+1))`). Edge-aligned was chosen for first
> bring-up so the frequency formula is unambiguous and the gate
> waveforms are simple to scope.

## Break input policy

* **TIM1 break is DISABLED.** No BKIN pin is wired on the board, so
  an enabled break with a floating BKIN line could permanently assert
  break and keep MOE off (no outputs).
* Do **not** re-enable break in CubeMX/code without configuring a
  physical BKIN pin tied to a safe inactive level. See
  `docs/KNOWN_RISKS.md`.

## Hall / RPM timestamp

* Hall edges are captured via **EXTI** on PB6/PB7/PB8 (rising+falling,
  pull-up). The EXTI ISR samples the Hall pins and TIM2 directly from
  registers (no HAL call) and stores a raw snapshot + timestamp +
  sequence counter. The debounce state machine runs in
  `HallSensor_Update()` from the main loop every iteration
  (single-writer architecture) — it is NOT gated by an EXTI pending
  flag, so a missed EXTI does not lose a stable transition (ISSUE-035).
* Timestamps use **TIM2**, a 32-bit free-running 1 MHz timer
  (prescaler 95 → 1 µs tick, period 0xFFFFFFFF). `App_GetMicros()`
  returns `TIM2->CNT`.
* The TIM1 PWM counter is **never** used for timestamps — it wraps
  every ~50 µs (20 kHz) and is not a monotonic timebase.
* RPM = `60 000 000 / (hall_period_us * 6 * MOTOR_POLE_PAIRS)`.
* `MOTOR_POLE_PAIRS = 15` (copied from the legacy firmware; verify
  with `identify` if the motor changes).

> The `.ioc` describes PB6/PB7/PB8 as a TIM4 Hall interface for
> CubeMX consistency. The runtime uses EXTI + TIM2 timestamps
> (see ISSUE-007). TIM4 is initialised but not used for RPM.

## What is NOT implemented (first bring-up scope)

* **No current sense, no current limiting, no ADC current
  measurement, no INA181 support.** Out of scope by design.
  Protection is only: firmware CCxE/CCxNE exclusion, Hall-based
  faults, the 6-step table, command watchdog, host-disconnect
  watchdog, and a current-limited bench PSU. Bench testing must
  start with the motor disconnected and a current-limited PSU.
* **Active brake disabled (coast).** `brake` / `x` performs coast
  (all gates off, motor free-spins). Active braking is disabled
  because there is no current sense and the power stage is
  unverified. Do not enable active brake until current sense exists.
* **Flash save is disabled.** `save` / `savecfg` / `saveall` return
  `[ERR] Flash storage disabled until safe implementation`. The
  previous 128 KB stack-buffer flash routine was a stack-overflow
  risk (ISSUE-011) and is removed. Load (`reload`) still works.
* **UART TX is DMA ring buffer.** `UartProtocol_Print()` writes to a
  512-byte TX ring buffer and kicks DMA1 Stream6. The main loop never
  blocks on TX (ISSUE-013). Dropped messages (TX ring full) are
  counted and reported in `status` as `TXDrops`.

## UART command set

Commands are case-insensitive, terminated with `\n` or `\r\n`.
Compatible with the H7 `motor_dispatcher` (which sends `rpm <signed>`,
`f<pwm>`, `b<pwm>`, `stop`, `identify`) and `tools/terminal.py`.

| Command                  | Behaviour                                             |
|--------------------------|--------------------------------------------------------|
| `f` / `forward`          | run forward at `default_pwm` (set via `defpwm <n>`)  |
| `b` / `backward`         | run reverse at `default_pwm`                          |
| `s` / `stop`             | **immediate safe stop** (all-off, cancels service/gate test, no fault latch) |
| `x` / `brake`            | **immediate safe stop** (same as stop in bring-up; active brake disabled) |
| `f<n>` / `b<n>`          | run with duty `n` (0..250, clamped)                   |
| `pwm` / `pwm <n>`        | query / set manual duty                               |
| `mode` / `mode duty` / `mode speed` | show / select mode                     |
| `mode normal` / `mode control` | legacy aliases: `duty` / `speed` (ISSUE-037)  |
| `rpm` / `rpm <signed>`   | show status / set RPM target (clamped to ±500)        |
| `pi <kp> <ki>` / `kp` / `ki` | set PI gains (clamped 0..10)                     |
| `base <lo> <mid> <hi>`   | feed-forward base PWM per RPM band (0..250)           |
| `boost <lo> <mid> <hi> <ms>` | start boost values (PWM 0..250, ms 0..1000)     |
| `ramp <up> <down>`       | speed-PI ramp rates (RPM/s)                           |
| `spstat`                 | speed-PI status                                       |
| `hall` / `h`             | raw and mapped Hall                                   |
| `map`                    | show active Hall map                                  |
| `map validate`           | validate active map                                   |
| `map edit`               | copy active map to candidate                          |
| `map set <raw> <sec>`    | edit candidate entry (raw 0..7, sector 0..5/invalid)  |
| `map candidate`          | show candidate map                                    |
| `map apply`              | apply candidate → active (validates first)            |
| `map discard`            | discard candidate                                     |
| `map default`            | load default map                                      |
| `map load`               | load from flash                                       |
| `map save`               | save to flash (disabled)                              |
| `mapreset`               | legacy: reset to default map                          |
| `reload`                 | load Hall map from flash (read-only, safe)           |
| `save` / `savecfg` / `saveall` | **disabled** (see above)                       |
| `identify` / `scan` / `test` | service routines (stop motor first)               |
| `gatetest <0-5> <1-100>` | single-sector scope test, 500 ms timeout (motor disconnected) |
| `kick on/off` / `ramp on/off` | duty-mode kick/ramp enable (ISSUE-038)         |
| `kickduty <n>` / `kickms <n>` | kick pulse duty / duration                   |
| `ramprate <n>` / `rampms <n>` | ramp step / interval                         |
| `defpwm <n>`             | default PWM for bare `f` / `b` (0..250)               |
| `status`                 | full status print (Hall, edges, duty, fault, TXDrops) |
| `estop`                  | emergency stop (fault latch, requires `clrerr`)       |
| `safe` / `alloff`        | same as `stop` (no fault latch)                       |
| `clrerr`                 | clear fault flags (required before new motion)        |
| `debug on/off`           | verbose debug toggle                                  |
| `dbg on/off`             | telemetry debug format                                |
| `telper <ms>`            | telemetry interval (20..5000 ms)                      |
| `help` / `?`             | command help                                          |

## Telemetry

Default cadence 100 ms (`telper <ms>` to change). Compact (default):

```
RPM:<m>,T:<t>,D:<d>,DIR:<F|R|N>,APP_PH:<p>,SP:<0|1>,BRAKE:<0|1>,FC:<c>,H:<h>,PWM_SET:<s>,PWM_ACT:<a>,QDROP:<q>
```

* `RPM` measured mechanical RPM · `T` = |target rpm| (speed mode) / 0
* `D` current applied duty (0..250) · `DIR` F/R/N · `APP_PH` app motor phase
* `SP` 1=speed mode · `BRAKE` 1=brake phase · `FC` fault code
* `H` raw Hall (0..7) · `PWM_SET` target duty (0..250) · `PWM_ACT` actual applied duty (0..250, never CCR ticks)
* `QDROP` command queue overflow drop count

The H7 wheelbridge prefixes each motor line as `FL|RPM:...`,
`FR|...`, `RL|...`, `RR|...` before forwarding to the PC.
`tools/terminal.py` parses that prefixed form. See `docs/PROTOCOL.md`.

## Hall Map Rules

* Raw 0 (0b000) and raw 7 (0b111) are **always invalid** — must map to 255.
* Raw 1..6 each map to a unique sector 0..5 (no duplicates, no gaps).
* `identify` rejects unstable Hall readings (hallA ≠ hallB).
* `identify` rejects raw 0 or 7 readings.
* Flash-loaded maps are validated with the same rules.
* `map set 0 <sector>` and `map set 7 <sector>` are rejected unless sector is `invalid`/`255`.

## Known limitations / bring-up warning

* **No hardware current measurement.** There is no current sense, no
  ADC current measurement, no INA181, no current limit, and no
  over-current fault in this revision. Protection is only: firmware
  CCxE/CCxNE exclusion, Hall-based faults (NO_HALL, INVALID_HALL,
  ILLEGAL_TRANSITION), command watchdog, host-disconnect watchdog,
  the 6-step table, and a current-limited bench PSU. **First tests
  must use a current-limited PSU with the motor disconnected.**
* TIM1 gate outputs are **not** hardware-verified yet.
* Dead-time is a software estimate, not scope-measured.
* Pole pairs (15) and the Hall map are copied from the legacy Arduino
  firmware and must be re-verified if the motor changes.
* **Any serious motor fault requires `clrerr` before new motion.**
  Faulted firmware cannot restart the motor until `clrerr` is sent.
  Faults: NO_HALL, INVALID_HALL, ILLEGAL_TRANSITION, HOST_LOST,
  WATCHDOG, HW_BREAK.
* **Speed mode requires periodic command heartbeat.** If H7/terminal
  stops sending `rpm <signed>` commands, the F411 stops/coasts within
  `CMD_WATCHDOG_MS` (800 ms) and raises `FAULT_WATCHDOG`. If the UART
  goes silent for `HOST_DISCONNECT_TIMEOUT_MS` (2000 ms),
  `FAULT_HOST_LOST` is raised. `mode speed` alone does not run the
  motor.
* **`brake` / `x` coast, they do not active-brake.** Active brake is
  disabled because there is no current sense and an unverified power
  stage. Do not enable active brake until current sense exists.
* **Flash `save` is disabled.** Config/Hall-map lives in RAM only
  until a safe flash implementation exists.
* **First hardware test must be with the motor disconnected and an
  oscilloscope/logic analyzer on the gate pins** — see
  `docs/BRINGUP.md`. Do not apply PWM to a motor until `allOff`,
  no same-phase overlap, and the sector truth table are scope-verified.
