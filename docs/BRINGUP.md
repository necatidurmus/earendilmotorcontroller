# Bring-up sequence — f411-motor-cube

Staged. Do not skip stages. Each stage has a "go/no-go" gate. If a
gate fails, stop and file an issue before continuing.

> The firmware is **not hardware-tested**. Scope verification is
> required before any motor is connected.

## Stage 0 — Build only

* `pio run -d f411-motor-cube`
* No hardware connected.
* Gate: clean build, no warnings about undefined symbols.

## Stage 1 — Flash and UART

* Flash via ST-Link. Open `pio device monitor` (115200 8N1).
* Send: `status`, `hall`, `help`.
* Expected: human-readable status block, `Hall=<n> State=<s>`, help
  text. Telemetry lines (`RPM:...,T:...,...`) appear every 100 ms.
* Gate: all three commands respond; telemetry format matches
  `docs/PROTOCOL.md`.

## Stage 2 — TIM1 no-load signal test (motor DISCONNECTED)

* **Motor disconnected.** PSU current limit 0.3 A. PSU on.
* Scope CH1 on PA8 (AH), CH2 on PA9 (BH), CH3 on PA10 (CH); second
  probe set on PA7/PB0/PB1 (AL/BL/CL) if available, or move probes
  phase by phase.
* Verify `allOff` at boot: every gate pin reads LOW (idle). Send
  `stop` and confirm again.
* Arm the gate test: `arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND`
* Verify the sector truth table: use `gatetest <sector> <duty>` to
  test one sector at a time:
  * `gatetest 0 10` — apply sector 0 at duty 10 for 100 ms
  * `gatetest 1 10` — apply sector 1
  * ... through `gatetest 5 10`
  * Or use `arm service` + `test` to cycle all 6 sectors
    automatically (duty 10, 300 ms each).
* For each sector verify: exactly one high-side PWMs, one *different*
  low-side is held ON, third phase floats.
* Verify **no same-phase high/low overlap** in any sector.
* Verify PWM frequency = 20 kHz on the high-side pins.
* Verify dead-time between a high-side and its complementary low-side
  on a phase that switches (adjust DTG if needed — ISSUE-018).
* Gate: no overlap, `allOff` clears all gates, 20 kHz confirmed.

## Stage 3 — Hall manual test

* Motor still connected only mechanically (electrically connected to
  the board for Hall, but **gate PSU off** or motor leads
  disconnected so it cannot spin under power).
* Rotate the wheel by hand slowly.
* Send `hall` repeatedly or watch telemetry `H:<hall>`.
* Verify the Hall sequence walks through six valid states
  (1,3,2,5,0,4 for the default map) with no 0b000/0b111 in steady
  rotation.
* Verify the edge counter increments (send `spstat` / watch `status`).
* Confirm pole pairs: one mechanical revolution = 6 × POLE_PAIRS
  edges. With POLE_PAIRS=15, one rev = 90 edges. Adjust
  `MOTOR_POLE_PAIRS` in `app_config.h` if the motor differs.
* Gate: valid Hall sequence, edge counter advances, RPM reads sane
  for a slow hand turn.

## Stage 4 — Duty mode, current-limited

* Motor connected, wheel off the ground. PSU 12 V, current limit
  0.5 A (raise cautiously).
* **Kick is DISABLED by default** (ISSUE-044).  There is no current
  sense, so a high-duty kick pulse could exceed the PSU current
  limit.  Do `kick off` explicitly to be sure, and keep `kickduty`
  low (≤60) if you later enable it.  `f10`, `f15`, `f20` must test
  the *actual* low duty, not a 225-duty kick.
* `mode duty`
* `kick off` — make sure the kick pulse is off.
* `f10` — watch the motor start slowly. Check PSU current.
  With kick off and ramp on (default), duty ramps up in small steps
  to 10, so the motor starts gently.
* `f15`, then `f20`.
* `stop` — motor coasts to a stop.
* `b10` — confirm reverse direction.
* Gate: motor spins in the commanded direction, PSU current stays
  within limit, `stop`/`s` coast, no fault in telemetry `FC`.
* Only after this stage passes, optionally try `kick on` with a low
  `kickduty 50` and re-test `f10` to see if the motor starts faster.
  If the PSU current-limits, revert to `kick off`.

## Stage 5 — Identify / map

* Only after Stage 4 is stable.
* `stop`, then arm the service:
  `arm service CURRENT_LIMITED_BENCH_SUPPLY`
* `identify` — the firmware toggles sectors and reads Hall to build
  a candidate map. Watch the `[ID]` lines and the
  `[IDENTIFY] candidate map:` output.
* Identify validates the candidate automatically:
  * If valid: `[IDENTIFY] validation: OK` and `[OK] Identify updated RAM hall map`.
  * If invalid (duplicate/missing sectors): `[ERR] Identify map rejected`
    and `[SAFE] Existing RAM hall map unchanged`.
* `map` to inspect the active map, source, and validity.
* `map validate` to double-check.
* If the identified map is wrong, `map default` restores the
  compile-time default.
* `save` is **disabled**. Map lives in RAM only until reset.
  A persistent map requires the safe flash storage implementation.
* Gate: `identify` produces a valid 6-entry map, or is rejected
  with a clear error. Active map is never corrupted by a bad
  identify.

## Stage 6 — Speed PI unloaded

* `mode speed`
* Defaults after ISSUE-040 are Kp=0.8, Ki=0.05, base=640/720/640,
  boost=880/960/1040 ms=150.  These are a middle ground between the
  too-conservative first cube revision and the aggressive legacy
  Arduino values.  Tune from here.
* `rpm 10` — motor regulates to ~10 RPM.
* `rpm 15`, then `rpm 23`.
* `rpm 0` or `stop` to stop.
* Watch `spstat` and telemetry: measured RPM should track target,
  `FC` stays 0, `PWM_ACT` is a real 0..4000 duty.
* Gate: stable regulation at 10/15/23 RPM, no runaway, no false
  `FAULT_NO_HALL`.

## Stage 7 — H7 bridge

* Connect the F411 command UART to the H7 motor UART.
* On the H7: `wheelbridge on`.
* From the H7 / terminal.py: command a forward RPM, e.g. `rpm 10`.
* Verify the PC sees `FL|RPM:12,T:10,D:34,DIR:F,APP_PH:1,SP:1,BRAKE:0,FC:0,H:5,PWM_SET:34,PWM_ACT:34`
  style lines.
* Verify `tools/terminal.py` populates the telemetry table for the
  correct motor prefix.
* Gate: prefixed telemetry parses in terminal.py.

## Stage 8 — Loaded test

* **Low RPM only.** Keep the bus voltage low and the PSU current
  limit conservative.
* Apply a small mechanical load.
* Raise `base`/`boost` slowly and in small steps only if the PI cannot
  hold the target.
* **Stop immediately** if the PSU current-limits (stop-the-line).
* Gate: stable regulation under light load at low RPM; no over-current.

## Stop-the-line criteria

Testing must stop immediately and the motor be disconnected if:

* PSU current limit / short mode
* MCU reset
* unexpected gate output during allOff
* same-phase high/low overlap
* invalid Hall state during stable rotation
* fault code appears repeatedly (FC != 0 in telemetry)
* motor stutters violently

## Fault policy (ISSUE-021)

* Any motor fault (NO_HALL, WATCHDOG, HOST_LOST, HW_BREAK,
  INVALID_HALL, ILLEGAL_TRANSITION, ESTOP, etc.) stops the motor
  immediately.
* Motion commands (`f`, `b`, `rpm`, `pwm`, `gatetest`, `identify`,
  `scan`, `test`) are **accepted** while a fault is displayed; the
  command clears the fault and resumes motion.
* `clrerr` can still be used to manually clear the fault and force
  STOPPED.
* Speed mode requires periodic `rpm <signed>` heartbeat from
  H7/terminal (ISSUE-020). If commands stop for 800 ms, motor stops.
* `mode speed` alone does not run the motor.
