# Safety — F411 BLDC power stage

This is power-electronics firmware. A mistake in the gate-drive logic
can shoot through a half-bridge and destroy the MOSFETs, the supply,
or both. Read this before connecting anything.

## Power-stage risk overview

The board drives a 3-phase BLDC hub motor with three half-bridges.
Each half-bridge has a high-side and a low-side MOSFET. If both
MOSFETs of the same half-bridge are ever ON at the same time, the
supply is shorted across that phase leg — this is **shoot-through**.

There is **no current sense, no current limiting, and no hardware
break input** in this revision. The only protections are:

* firmware keeping CCxE and CCxNE mutually exclusive per phase,
* the 6-step table guaranteeing the high phase ≠ the low phase,
* a current-limited bench PSU,
* you starting with the motor disconnected.

## What shoot-through is

Shoot-through = both the high-side and low-side of the same phase
conduct simultaneously, forming a low-impedance path from the supply
rail to ground through that leg. It causes a large current spike that
trips the PSU current limit or destroys the MOSFETs/driver.

It can happen because of:

* enabling both `CCxE` and `CCxNE` for the same phase,
* a commutation table that maps the high and low of a step to the same
  phase,
* dead-time too small for the actual gate driver switching speed,
* reconfiguring a channel's OCxM while its output enable is still on
  (transient overlap).

## Why CCxE / CCxNE correctness matters

On the STM32F411 TIM1, each channel pair has two output-enable bits:

* `CCxE`  — enables the high-side output (`OCx`, the CHx pin)
* `CCxNE` — enables the low-side complementary output (`OCxN`, the
  CHxN pin)

The firmware must never set both for the same phase in the same step.
See `docs/TIM1_GATE_DRIVE.md` for the exact per-sector truth table
and the code in `App/Src/motor_driver.c`. Any change to the gate
hot path must explain its CCxE/CCxNE effect in the change description
and in that doc.

## Stop / Brake / Emergency Stop

| Command | Behavior | Fault latch | Restart |
|---------|----------|-------------|---------|
| `stop` / `s` | Immediate coast stop, all-off, cancels service/gate test | No | `f`/`b`/`rpm` |
| `brake` / `x` | **Active brake**: all low-side MOSFETs ON, windings shorted | No | `f`/`b`/`rpm` |
| `estop` | Emergency stop with fault display | No (clears on next command) | `f`/`b`/`rpm` |
| `safe` / `alloff` | Same as stop (no fault) | No | `f`/`b`/`rpm` |

* `stop` and `safe` cancel any active service task or gate test
  immediately and coast.
* `brake` performs active braking: all three low-side MOSFETs are turned
  ON and the motor windings are shorted together. This produces a rapid
  deceleration and can create large currents. **Only use with a
  current-limited bench supply, motor unloaded or lightly loaded, and
  after gate-drive verification.** At high speed the back-EMF can trip
  the PSU or overstress the MOSFETs.
* `brake` holds for `brake_hold_ms` (default 3000 ms) then automatically
  releases to coast.
* `estop` raises a fault and stops the motor, but faults are no longer
  latched; the next motion command clears it and resumes.

## What to do if the PSU enters current-limit / short mode

1. **Stop.** Disconnect the motor. Cut the PSU output.
2. Do not retry power until you have found the cause.
3. Re-verify `allOff` and the sector truth table with the motor
   disconnected and the scope on the gate pins (BRINGUP Stage 2).
4. Log a hardware-safety issue (`.github/ISSUE_TEMPLATE/hardware_safety_issue.yml`).

A current-limit trip is a stop-the-line event, not a normal part of
tuning.

## Mandatory pre-motor checklist

Before connecting any motor:

* [ ] `pio run -d f411-motor-cube` builds clean.
* [ ] Flash the firmware; `status`, `hall`, `help` respond on UART.
* [ ] Motor **disconnected**. PSU current limit set low (e.g. 0.3–0.5 A).
* [ ] Scope/logic analyzer on all six gate pins (CH1/CH1N, CH2/CH2N,
      CH3/CH3N) and on the phase outputs.
* [ ] `allOff` verified: every gate pin reads LOW.
* [ ] No same-phase high/low overlap in any sector.
* [ ] Manual Hall wheel turn increments the edge counter; RPM sane.
* [ ] TIM1 break is disabled and no unexpected break events occur.

Only after all of the above may you proceed to a current-limited duty
test with the motor connected (BRINGUP Stage 4).

## Stop-the-line conditions

Stop immediately and disconnect the motor if:

* the PSU enters current-limit / short mode,
* same-phase high/low overlap is observed on the scope,
* an unexpected TIM1 break event occurs (break is disabled — any break
  is a fault),
* an invalid Hall state (0b000 or 0b111) appears (which causes immediate all-off),
* the MCU resets or HardFaults during motor operation,
* a gate output is driven when the firmware reports `allOff`,
* the motor runs in the wrong direction for the commanded sign,
* telemetry RPM disagrees wildly with an independent tachometer.

## Safe bench setup

* **Current-limited PSU.** Set the current limit low (start 0.3–0.5 A)
  and the voltage low if possible (e.g. 12 V, not 24 V, for first
  tests).
* **Lower voltage if possible.** Lower bus voltage means lower energy
  in a shoot-through event.
* **Motor mechanically unloaded.** Wheel off the ground / motor not
  driving a load.
* **Oscilloscope / logic analyzer** on the gate pins and phase
  outputs, with common ground referenced correctly.
* **Common ground.** PSU ground, board ground, scope ground, and
  ST-Link ground must be common. Do not float the scope ground lead
  on a live phase — that shorts the phase through the scope.
* **No 300–400 RPM tests before low-speed stability.** Do not exceed
  ~30 RPM until `rpm 10` / `rpm 15` are stable with sane telemetry.
  High-speed tests are Phase 7, not first bring-up.
