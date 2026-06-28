# ROADMAP.md

## Goal

Reliable STM32Cube-based F411 BLDC motor controller with safe TIM1
gate control, Hall commutation, RPM telemetry, and H7 compatibility.

## Phase 0 — Repository cleanup and documentation

Deliverables:

* `AGENTS.md`, `ISSUES.md`, `ROADMAP.md`
* `docs/` (SAFETY, BRINGUP, TIM1_GATE_DRIVE, PROTOCOL, KNOWN_RISKS)
* `.gitignore`
* `.github/ISSUE_TEMPLATE/*`
* remove `.pio/` artifacts and `*.zip` from the repo

Exit criteria:

* repo contains source/config/docs only
* known risks documented
* no build artifacts tracked

Status: **done** (this revision).

## Phase 1 — Build and CubeMX/PlatformIO consistency

Deliverables:

* `platformio.ini` builds with `pio run -d f411-motor-cube`
* `.ioc` consistent with code (TIM1 mode, break policy, TIM2 added)
* no invalid F411 peripherals in the active generated path (TIM6 is a
  no-op stub; scheduler tick is SysTick)
* TIM1 / TIM2 / USART2 configuration documented in README and
  `docs/TIM1_GATE_DRIVE.md`

Exit criteria:

* PlatformIO build passes
* no generated/user-code conflict
* `HAL_TIM_PWM_MspInit` used for TIM1, `HAL_TIM_Base_MspInit` for TIM2/TIM4

Status: **done** (this revision — build passes).

## Phase 2 — Safe TIM1 gate-control core

Deliverables:

* correct TIM1 PWM MspInit (clock enable)
* `HAL_TIM_MspPostInit(&htim1)` called (GPIO AF on gate pins)
* correct CCxE/CCxNE logic: high PWM = CCxE, low ON = CCxNE, float = none
* correct CCMR channel-specific masks (no shifted OC1PE)
* `allOff` / `FaultOff` clear every CCxE/CCxNE bit
* no active brake
* correct PWM frequency (edge-aligned, ARR=4799 → 20 kHz)
* break input disabled (no BKIN wired)

Exit criteria:

* motor-disconnected gate test checklist passes (scope)
* no high/low same-phase overlap in the truth table
* `allOff` verified on scope (all gate pins low)

Status: **code complete, NOT hardware-verified.** Scope verification
required before Phase 3.

## Phase 3 — Hall sensor and RPM timestamp

Deliverables:

* remove TIM1 counter from Hall timestamp (done — uses TIM2)
* TIM2 32-bit 1 MHz micros timer (done)
* correct Hall period / RPM calculation (done)
* Hall freshness logic fixed (startup timeout, `has_ever_run`)
* `POLE_PAIRS = 15` documented and flagged for re-verification
* EXTI on PB6/PB7/PB8 for immediate Hall edge capture (done — ISSUE-007)
* DMA TX ring buffer to avoid blocking UART TX (done — ISSUE-013)

Exit criteria:

* manual Hall transitions produce valid edge count and measured RPM
* 10 RPM with 15 pole pairs reads sane (≈15 edges/s, period ≈66 ms)

Status: **code complete, NOT hardware-verified.**

## Phase 4 — Duty mode bring-up

Deliverables:

* `f` / `b` / `f<pwm>` / `b<pwm>` / `stop` work
* startup no-Hall timeout fixed (`START_NO_HALL_TIMEOUT_MS`, `has_ever_run`)
* direction handling fixed (`f` after `b` no longer inherits reverse)
* fault behaviour correct (FaultManager drives `FaultOff`)

Exit criteria:

* low PWM duty mode ready for current-limited bench test

Status: **code complete, NOT hardware-verified.**

## Phase 5 — Speed PI mode

Deliverables:

* `rpm <signed>` starts `PHASE_RUNNING` (run_request path fixed)
* SpeedPI fault integrates with FaultManager (SPD_FAULT_NO_HALL → FAULT_NO_HALL)
* `base` / `boost` / `ramp` / `pi` commands work
* conservative defaults (Kp=0.8, Ki=0.05, max PWM=180, duty 0..250)
* telemetry reports target / measured / duty / fault
* command watchdog requires real heartbeat (ISSUE-020)
* fault latching requires `clrerr` (ISSUE-021)

Exit criteria:

* unloaded `rpm 10` / `rpm 15` / `rpm 23` bench test plan ready

Status: **code complete, NOT hardware-verified.**

## Phase 6 — H7 and terminal integration

Deliverables:

* H7 command compatibility preserved (`rpm`, `f`, `b`, `stop`, `identify`)
* telemetry format works with the H7 wheelbridge prefix
* `tools/terminal.py` can show actual measured RPM

Exit criteria:

* `FL|RPM:...,T:...` style telemetry is parseable by terminal.py

Status: **code complete, NOT hardware-verified.** No protocol fields
changed; `PWM_ACT` is now a real 0..250 duty (was truncated CCR ticks).

## Phase 7 — Hardware bring-up

Deliverables:

* scope-verified TIM1 outputs (CH1/CH1N, CH2/CH2N, CH3/CH3N)
* motor-disconnected tests pass
* low-voltage / current-limited duty tests
* Hall map `identify`
* RPM PI tuning

Exit criteria:

* no PSU short / current-limit trips at low PWM
* RPM measurement agrees with an independent measurement

Status: **not started.** Blocked on Phase 2 scope verification.

## Stop-the-line criteria

Testing must stop immediately and the motor be disconnected if any of
these occur:

* PSU enters current-limit / short mode
* same-phase high/low overlap observed on the scope
* unexpected TIM1 break event (break is disabled — any break is a fault)
* invalid Hall state (0b000 or 0b111) during stable rotation
* MCU reset / HardFault during motor operation
* a gate output is driven when the firmware reports `allOff`
* motor runs in the wrong direction for the commanded sign
* telemetry RPM disagrees wildly with an independent tachometer
