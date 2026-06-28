# TIM1 gate drive — f411-motor-cube

How the STM32F411 TIM1 advanced-control timer drives the three
half-bridges. Read this before touching `App/Src/motor_driver.c`.

## Pin / channel mapping

| Phase | High-side (H) | Low-side (L) | TIM channel        | CCxE  | CCxNE |
|-------|---------------|--------------|--------------------|-------|-------|
| A     | PA8  = AH     | PA7 = AL     | TIM1_CH1 / CH1N    | CC1E  | CC1NE |
| B     | PA9  = BH     | PB0 = BL     | TIM1_CH2 / CH2N    | CC2E  | CC2NE |
| C     | PA10 = CH     | PB1 = CL     | TIM1_CH3 / CH3N    | CC3E  | CC3NE |

This pin assignment is copied verbatim from the legacy Arduino
firmware (`f411-motor/src/main.cpp`):

```
AH_PIN PA8, AL_PIN PA7, BH_PIN PA9, BL_PIN PB0, CH_PIN PA10, CL_PIN PB1
```

## CCxE vs CCxNE — the model

For each channel pair, TIM1 has two output-enable bits in `CCER`:

* `CCxE`  — enables the main output `OCx` (the high-side pin, CHx).
* `CCxNE` — enables the complementary output `OCxN` (the low-side
  pin, CHxN).

`OCxN` is the complement of the compare reference `OCxREF` (with
dead-time insertion). The output-compare mode `OCxM` (in `CCMR1`/`CCMR2`)
selects what `OCxREF` does:

* `OCxM = 110` (PWM mode 1): `OCxREF` is high while `CNT < CCR`.
* `OCxM = 100` (forced inactive): `OCxREF` is held low → `OCxN` is
  held **high** (active), `OCx` is held low (inactive).
* `OCxM = 101` (forced active): `OCxREF` high → `OCx` high, `OCxN` low.

## Asynchronous six-step behaviour (first bring-up)

We deliberately do **not** use synchronous complementary PWM (where
both `CCxE` and `CCxNE` are on and the low-side switches as the
complement of the high-side PWM). Instead, per electrical sector:

| Role           | OCxM            | CCxE | CCxNE | CCRx     | Effect                           |
|----------------|-----------------|------|-------|----------|----------------------------------|
| High PWM phase | PWM1 (110)      | 1    | 0     | duty     | high-side PWMs, low-side OFF     |
| Low ON phase   | forced inactive (100) | 0 | 1    | —        | low-side held ON (OCxN high)     |
| Floating phase | forced inactive (100) | 0 | 0    | —        | both OFF (Hi-Z)                  |

Invariant: **`CCxE` and `CCxNE` are never both set for the same phase
in the same step.** The 6-step table additionally guarantees
`high_phase != low_phase` for every sector, so same-phase
shoot-through is impossible by construction.

The low-side is held statically ON (not PWM-modulated). This matches
the legacy Arduino firmware's `digitalWrite(PH_LOW[x], HIGH)` intent,
now done through the proper complementary-output enable instead of
Arduino GPIO.

## Sector truth table (forward)

| Sector | High PWM | Low ON | Floating |
|--------|----------|--------|----------|
| 0      | B        | A      | C        |
| 1      | C        | A      | B        |
| 2      | C        | B      | A        |
| 3      | A        | B      | C        |
| 4      | A        | C      | B        |
| 5      | B        | C      | A        |

Reverse direction = the same table shifted by 3 sectors (180
electrical degrees).

### Cross-check against the legacy Arduino firmware

The legacy firmware (`f411-motor/src/main.cpp`) used:

```
PH_HIGH[6] = {BH, CH, CH, AH, AH, BH}   // {B, C, C, A, A, B}
PH_LOW [6] = {AL, AL, BL, BL, CL, CL}   // {A, A, B, B, C, C}
```

Encoding A=0, B=1, C=2, that is exactly:

```
fwd_high = {1, 2, 2, 0, 0, 1}   // B, C, C, A, A, B
fwd_low  = {0, 0, 1, 1, 2, 2}   // A, A, B, B, C, C
```

which is the table above. The cube `motor_driver.c` uses the same
table. **If a hardware `identify` run produces a different mapping,
update `DEFAULT_HALL_MAP_*` in `app_config.h` and document the
reason here — do not silently change the phase order.**

> The table above is the *validated* table from the legacy Arduino
> firmware. The legacy firmware drove the low-side with
> `digitalWrite` (plain GPIO) and the high-side with `analogWrite`
> (timer PWM). The cube firmware drives both through TIM1
> complementary outputs — same logical table, proper timer hardware.

## `allOff` behaviour

`MotorDriver_AllOff()` / `MotorDriver_FaultOff()` call `phase_all_off()`
which, for each of the three phases:

1. clears `CCxE` and `CCxNE` in `CCER`,
2. writes `OCxM = forced inactive` (so `OCxREF = 0`).

`MotorDriver_AllOff()` leaves `MOE` set so the bridge can re-arm
after a `clrerr`; the per-phase enable bits are the real cut-off.

`MotorDriver_FaultOff()` additionally clears `MOE` for maximum cutoff.
`MOE` is re-enabled automatically by `MotorDriver_ApplyStep()` on the
next normal commutation request (after `clrerr` and a new motion command).

With `OSSI/OSSR` **disabled** (ISSUE-029/034, the active configuration),
disabled channels go to the **OFF state (Hi-Z)** per the RM0090 truth
table — **not** to their idle level. `OCxIDLE = RESET` is set but only
takes effect when `OSSI/OSSR = 1`. Whether the gate-driver input is
actually held LOW (rather than floating) depends on the board's
pull-down network or the gate driver's own input state. **Verify on a
scope at BRINGUP Stage 2 that all six gate pins are LOW when `allOff`
is called.** If they float, evaluate enabling `OSSI=1` / `OSSR=1`
(ISSUE-034) so disabled channels are actively driven to the idle (low)
level.

> The legacy Arduino firmware used `analogWrite(pin, 0)` +
> `digitalWrite(pin, LOW)` which forces the GPIO to a known LOW.
> The cube firmware's `allOff` relies on the TIM1 output enable
> clearing + OSSI/OSSR policy. With OSSI/OSSR disabled this is Hi-Z,
> not a forced LOW. The practical gate level depends on the hardware.

## Sector transition — unchanged phase (ISSUE-028)

`MotorDriver_ApplyStep()` only reconfigures the phase that actually
changes between adjacent sectors. The 6-step table changes exactly
one of (high PWM, low ON) per transition; the other is left
continuously driven:

* If `new_low != s_last_low_idx`: `phase_low_on(new_low)`.
  Otherwise the existing low-side stays statically ON (no
  disable/enable cycle, no gate glitch).
* If `new_high != s_last_high_idx`: `phase_high_pwm(new_high, ccr)`.
  Otherwise only `CCR` is refreshed (`*ccr = s_ccr_ticks`) so a duty
  change takes effect without disabling/re-enabling the high-side.

The previous phase is still disabled first (`phase_disable`) before
the new one is enabled, preserving the no-overlap ordering. This
matches the legacy Arduino firmware's `if (oldH != newH)` /
`if (oldL != newL)` guards and eliminates the torque ripple / gate
glitch that the unconditional disable/enable caused.

## Dead-time / blanking

* `BDTR.DTG = 63` → ~0.66 µs at 96 MHz. **Not bench-verified**
  (ISSUE-018). Because the low-side is static ON and the high-side is
  the only switching output per phase, dead-time matters mainly on
  phase transitions; it must still be confirmed on a scope.
* `OSSI` / `OSSR` are **disabled** (ISSUE-029). Per RM0090, with
  `OSSR = 0` a channel whose enable bits (`CCxE`/`CCxNE`) are both
  clear goes to the **OFF state (Hi-Z)**, not its idle level. The
  `OCxIDLE = RESET` idle level only applies when `OSSI/OSSR = 1`.
  Disabled-phase gate-driver inputs therefore float; ISSUE-034
  tracks the future safety improvement of enabling OSSI/OSSR so
  disabled channels are actively driven LOW. This is conservative
  for first bring-up and matches the active `tim.c`.

## Break input policy

* **Break is disabled.** No BKIN pin is wired (ISSUE-005). With break
  enabled and BKIN floating, break could permanently assert and keep
  `MOE` off.
* Do not re-enable break without:
  * a BKIN pin configured in CubeMX/`.ioc` as `TIM1_BKIN` AF,
  * a defined polarity and pull (pull to the inactive level),
  * the physical connection documented here.
* The `TIM1_BRK_TIM9_IRQHandler` still calls `App_Tim1BrkIsr()` →
  `FaultManager_Raise(FAULT_HW_BREAK)`, so if break is ever wired and
  trips, it is treated as a fault.

## PWM frequency formula

TIM1 is on APB2, timer clock = 96 MHz. Edge-aligned, upcounting:

```
f_pwm = TIM_CLK / (ARR + 1) = 96 MHz / (4799 + 1) = 20.0 kHz
```

`ARR = 4799`, prescaler 0, `PWM_PERIOD_TICKS = 4799` in `app_config.h`.
User duty 0..4000 maps to `CCR = duty * (ARR + 1) / PWM_MAX_DUTY`.
Duty 4000 maps to CCR = 4799 = ARR (~100 % duty).

> Center-aligned mode would give `f = TIM_CLK / (2*(ARR+1)) = 10 kHz`
> for the same ARR. Edge-aligned was chosen for first bring-up so the
> frequency is unambiguous and gate waveforms are simple to scope.

## CCMR bit masks (why the old code was wrong)

The original `motor_driver.c` did:

```c
ccmr_val &= ~((uint32_t)TIM_CCMR1_OC1PE << (oc_mode_shift));
```

`TIM_CCMR1_OC1PE` is bit 3 (channel-1 preload). Shifting it by
`oc_mode_shift` (e.g. 12 for channel 2) wrote bit 15 — a reserved
position — instead of `OC2PE` (bit 11). This corrupted unrelated
CCMR bits and never set the intended preload.

The rewrite uses channel-specific masks from a `PhaseReg` table:

* CH1: `TIM_CCMR1_OC1M` (0x70), `TIM_CCMR1_OC1PE` (0x08), shift 4
* CH2: `TIM_CCMR1_OC2M` (0x7000), `TIM_CCMR1_OC2PE` (0x0800), shift 12
* CH3: `TIM_CCMR2_OC3M` (0x70), `TIM_CCMR2_OC3PE` (0x08), shift 4

No channel-1 mask is ever shifted by another channel's offset.

## Why `analogWrite` / `digitalWrite` is forbidden

The legacy Arduino firmware used `analogWrite` for the high-side PWM
and `digitalWrite` for the low-side ON/OFF. This is forbidden in the
cube firmware because:

* `analogWrite`/`digitalWrite` hide the timer/channel/CCER state and
  mix Arduino GPIO with timer AF on the same pins, making shoot-through
  protection impossible to reason about.
* The cube firmware uses `framework = stm32cube`, not Arduino, and
  must drive the advanced timer's complementary outputs through
  `CCxE`/`CCxNE` directly.
* The hot path must be deterministic register access, not framework
  calls that may reconfigure pins.

Grep the repo: `analogWrite` / `digitalWrite` may only appear in the
legacy `f411-motor/` tree and in comments — never in `App/` or `Core/`.
