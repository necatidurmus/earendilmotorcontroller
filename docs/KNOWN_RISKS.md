# Known risks & unverified assumptions — f411-motor-cube

Hardware bring-up was completed 2026-07-01: motor ran at low RPM
with current-limited PSU, no excessive current, active brake worked.
Items below that were previously "unverified" are now marked
**VERIFIED** where the hardware test confirmed them.

## No current sense / current limiting

* This hardware revision does not use current sense. There is **no
  ADC current measurement, no INA181, and no over-current fault**.
  This is by design.
* Protection is provided through Hall timeout, fault state, watchdog,
  and manual stop behaviors. Use a current-limited bench supply when
  operating the motor.
* `brake` / `x` performs active braking (all low-side MOSFETs ON).
  Without current sense there is no protection against the current
  spike caused by rapid deceleration. Use only with a current-limited
  bench supply and at low speed.
* Bench testing must start with the motor disconnected and a
  current-limited PSU (0.3–0.5 A). See `docs/SAFETY.md`.

## Pole pairs

* `MOTOR_POLE_PAIRS = 15` is copied from the legacy Arduino firmware.
* **VERIFIED (hw)**: motor rotated correctly with this value at low
  RPM, suggesting 15 pole pairs is correct (or close enough for
  commutation). Confirm precisely with `identify` if needed.

## Hall map may need `identify`

* `DEFAULT_HALL_MAP_0..7 = {255, 1, 3, 2, 5, 0, 4, 255}` is copied
  from the legacy firmware. If the motor/wiring differs, the map must
  be re-derived with `identify`.
* `save` is disabled, so a re-identified map lives in RAM only until
  reset.
* **Wrong Hall map is dangerous**: incorrect sector mapping causes
  the firmware to energize the wrong phases, leading to high current,
  vibration, MOSFET stress, or motor stall. Always validate the map
  before extended operation.
* `identify` itself energizes motor phases and requires a
  current-limited bench supply and service arming. See
  `docs/HALL_IDENTIFY.md`.
* The identify candidate is now validated before being applied to the
  active map. A bad identify (duplicate/missing sectors) is rejected
  and the existing map is preserved.
* `identify` now rejects unstable Hall readings (hallA ≠ hallB) and
  invalid raw Hall codes (0b000=0, 0b111=7).  These steps are skipped
  and the resulting incomplete map is rejected by the validator.
* Raw 0 and raw 7 are always invalid in any Hall map.  `Commutation_
  SetMapEntry()` rejects writing a valid sector to raw 0/7.
* Flash-loaded maps are validated with the same strict rules.

## Gate driver assumptions

* **VERIFIED (hw)**: gate driver accepts TIM1 3.3 V logic levels on
  PA7/8/9/10 and PB0/1. Motor ran smoothly with the documented
  dead-time.
* Dead-time `DTG = 63` (~0.66 µs @ 96 MHz) — **VERIFIED (hw)**:
  no shoot-through observed, motor ran smoothly at low RPM.
* The high-side/low-side pin assignment (AH=PA8, AL=PA7, BH=PA9,
  BL=PB0, CH=PA10, CL=PB1) is taken from the legacy firmware and
  **VERIFIED (hw)**: motor commutated correctly with this mapping.

## Break input not wired

* TIM1 break is **disabled**. There is no hardware over-current /
  over-voltage / fault line wired to BKIN.
* Any hardware fault (shoot-through, over-temp) has no automatic
  hardware cutoff. The firmware can only disable `CCxE`/`CCxNE` in
  response to *software-detected* conditions (Hall loss, watchdog).
* Re-enabling break requires a BKIN pin configured and tied to a safe
  inactive level (ISSUE-005).

## TIM1 outputs scope-verified

* **VERIFIED (hw)**: CCxE/CCxNE logic, sector table, and `allOff`
  confirmed by motor operation at low RPM. No shoot-through, no
  excessive current, motor commutated correctly.

## Storage save — ISSUE-011 RESOLVED

* `save` / `savecfg` / `saveall` / `map save` are now **enabled** (ISSUE-011 fixed).
* Append-only record scheme with FNV-1a CRC32. No 128 KB buffer.
* Hall map is preserved during config area compaction.
* Motor must be stopped before save/erase. See `docs/F411_FLASH_CONFIG_PERSISTENCE.md`.

## UART TX is DMA ring buffer (ISSUE-013)

* `UartProtocol_Print()` writes bytes into a 512-byte TX ring buffer
  and kicks DMA1 Stream6. The main loop never blocks on TX.
* RX path: DMA1 Stream5 circular mode + IDLE interrupt.
* See `App/Src/uart_protocol.c` for details.

## Hall sensing uses EXTI + TIM2 (ISSUE-007, ISSUE-035)

* **VERIFIED (hw)**: Hall sensor readings valid, RPM telemetry sane
  at low speed.

## Speed mode requires command heartbeat (ISSUE-020)

* Both duty and speed modes require real command refresh.
* A single `rpm 23` will NOT keep the motor running indefinitely.
* H7/terminal must send periodic `rpm <signed>` commands.
* If commands stop for `CMD_WATCHDOG_MS` (800 ms), motor stops.
* `mode speed` alone does not run the motor.

## Fault latching (ISSUE-021)

* All faults stop the motor immediately, but the fault state is **not
  latched**.
* A new motion command (`f`, `b`, `rpm`, `pwm`) clears the displayed
  fault, releases the safety lock, and resumes motion.
* `clrerr` can still be used to manually clear the fault and force
  STOPPED.

## 300–400 RPM not yet tested

* Low-RPM tests passed (motor ran smoothly). Higher RPM tests
  are pending.

## `.ioc` cleaned but hand-maintained (ISSUE-016, ISSUE-D)

* The `.ioc` has been cleaned: TIM6 removed, invalid F411 RCC fields
  removed, TIM6_DAC_IRQn removed.
* TIM4 is kept for CubeMX consistency but is not used at runtime.
* Regenerating in CubeMX without care could still overwrite the
  hand-edited `tim.c` (edge-aligned, break disabled, TIM2 micros).
