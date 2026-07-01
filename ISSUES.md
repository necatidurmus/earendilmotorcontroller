# ISSUES.md

Tracked issue register for the `f411-motor-cube` firmware. Each issue
lists severity, status, affected files, description, evidence, fix
plan, and acceptance criteria. Add new issues here rather than hiding
assumptions in code.

Status legend: **OPEN** · **FIXED (code)** · **VERIFIED (hw)** · **WONTFIX**

---

## ISSUE-001 — TIM1 MSP / PostInit missing or wrong

* Severity: Critical
* Status: **FIXED (code)** — not hardware-verified
* Affected: `Core/Src/tim.c`
* Description: TIM1 is initialised with `HAL_TIM_PWM_Init()`, so the
  HAL callback that runs is `HAL_TIM_PWM_MspInit()` — not
  `HAL_TIM_Base_MspInit()`. The original code only implemented
  `HAL_TIM_Base_MspInit`, so the TIM1 clock was never enabled. Also
  `HAL_TIM_MspPostInit()` was defined but never called, so the gate
  pins (PA7/8/9/10, PB0/1) were never configured as TIM1 AF.
* Evidence: `tim.c` defined `HAL_TIM_Base_MspInit` for TIM1 and
  `HAL_TIM_MspPostInit` but `MX_TIM1_Init` never called the latter.
* Fix: implemented `HAL_TIM_PWM_MspInit` (enables `TIM1_CLK`), keep
  `HAL_TIM_Base_MspInit` for TIM2/TIM4, and call
  `HAL_TIM_MspPostInit(&htim1)` at the end of `MX_TIM1_Init`.
* Acceptance: build passes; scope shows TIM1 waveforms on gate pins
  (Phase 2 hardware check).

---

## ISSUE-002 — CCxE / CCxNE high-side / low-side mix-up

* Severity: Critical
* Status: **FIXED (code)** — not hardware-verified
* Affected: `App/Src/motor_driver.c`
* Description: The low-side drive used `CCxE` (the high-side enable)
  instead of `CCxNE`, and `force_high_pwm` enabled **both** `CCxE`
  and `CCxNE` for the PWM phase (synchronous complementary PWM), which
  is explicitly forbidden for first bring-up.
* Evidence: `force_low` set `ccer_bit = TIM_CCER_CC1E`; `force_high_pwm`
  did `*ccer |= (ccer_bit | ccer_n_bit)`.
* Fix: rewritten with explicit per-channel descriptors. High PWM =
  `CCxE` only with `OCxM=PWM1`; low ON = `CCxNE` only with
  `OCxM=forced inactive` (OCxN held high); floating = both clear.
  `phase_high_pwm`/`phase_low_on` never set both enables for a phase.
* Acceptance: static truth table shows no sector enables CCxE and
  CCxNE for the same phase; `allOff` clears all six enable bits.

---

## ISSUE-003 — CCMR bit masks are wrong

* Severity: Critical
* Status: **FIXED (code)**
* Affected: `App/Src/motor_driver.c`
* Description: Code shifted `TIM_CCMR1_OC1PE` (bit 3, channel-1
  preload) by another channel's `OCxM` field offset (e.g. 12 for
  channel 2). This wrote bit 15 (reserved) instead of the OC2PE bit
  (bit 11), corrupting unrelated CCMR bits and not setting OCxPE.
* Evidence: `ccmr_val &= ~((uint32_t)TIM_CCMR1_OC1PE << (oc_mode_shift))`.
* Fix: channel-specific masks via a `PhaseReg` table
  (`TIM_CCMR1_OC1M`/`OC1PE`, `TIM_CCMR1_OC2M`/`OC2PE`,
  `TIM_CCMR2_OC3M`/`OC3PE`). No cross-channel shifting.
* Acceptance: review confirms each channel's OCxM and OCxPE bits are
  written with the correct field mask.

---

## ISSUE-004 — TIM1 center-aligned PWM frequency mismatch

* Severity: High
* Status: **FIXED (code)**
* Affected: `Core/Src/tim.c`, `App/Inc/app_config.h`, `README.md`
* Description: ARR=6399 in center-aligned mode gives
  `96 MHz / (2 * 6400) = 7.5 kHz`, not 15 kHz. The comment claimed 15 kHz.
* Evidence: `htim1.Init.CounterMode = TIM_COUNTERMODE_CENTERALIGNED1`
  with `Period = 6399`.
* Fix: switched to edge-aligned upcounting (`TIM_COUNTERMODE_UP`),
  ARR=6399 → `96 MHz / 6400 = 15.0 kHz`. `PWM_PERIOD_TICKS` (6399)
  unchanged. `.ioc` updated to `TIM_COUNTERMODE_UP`. Formula documented
  in `docs/TIM1_GATE_DRIVE.md` and README.
* Acceptance: scope shows 15 kHz PWM; `PWM_PERIOD_TICKS` matches ARR.

---

## ISSUE-005 — Break input enabled without configured hardware

* Severity: Critical
* Status: **FIXED (code)** — not hardware-verified
* Affected: `Core/Src/tim.c`, `f411-motor-cube.ioc`
* Description: TIM1 break was enabled with no BKIN pin configured/wired.
  With break polarity active-low and a floating BKIN, break could
  permanently assert and keep MOE off (no gate outputs), or trip
  unpredictably.
* Evidence: `bd.BreakState = TIM_BREAK_ENABLE` with no BKIN GPIO config.
* Fix: `bd.BreakState = TIM_BREAK_DISABLE`. `.ioc` documents that
  BreakPolarity/BreakFilter are placeholders only. Re-enabling requires
  a physical BKIN pin tied to a safe inactive level.
* Acceptance: no unexpected break events; `docs/KNOWN_RISKS.md` lists
  "no hardware break protection" as a risk.

---

## ISSUE-006 — Hall / RPM timestamp uses TIM1 PWM counter

* Severity: Critical
* Status: **FIXED (code)** — not hardware-verified
* Affected: `App/Src/hall_sensor.c`
* Description: `__HAL_TIM_GET_COUNTER(&htim1)` was used for Hall edge
  timestamps. TIM1 is the 15 kHz PWM counter — it wraps every ~67 µs
  and is not a monotonic timebase, so Hall period and RPM were invalid.
* Evidence: four `__HAL_TIM_GET_COUNTER(&htim1)` calls in hall_sensor.c.
* Fix: added TIM2 (32-bit, 1 MHz, free-running) in `tim.c`; all
  timestamps use `App_GetMicros()` (= `TIM2->CNT`). Unsigned 32-bit
  subtraction handles wrap.
* Acceptance: manual Hall turn increments the edge counter; 10 RPM
  with 15 pole pairs (≈66 ms edge period) reads sane.

---

## ISSUE-007 — TIM4 Hall interface vs GPIO polling inconsistency

* Severity: High
* Status: **FIXED (code)** — not hardware-verified
* Affected: `f411-motor-cube.ioc`, `Core/Src/gpio.c`, `Core/Src/tim.c`,
  `App/Src/hall_sensor.c`
* Description: The `.ioc` configured PB6/PB7/PB8 as `TIM4_CH1/2/3` for
  a TIM4 Hall interface, but `gpio.c` configured them as plain GPIO
  inputs and `hall_sensor.c` polled them. TIM4 was initialised but not
  used for RPM. Pure polling could miss edges at high RPM when UART TX
  blocked (see ISSUE-013).
* Evidence: `.ioc` `PB6.Signal=S_TIM4_CH1` etc.; `gpio.c` set
  `GPIO_MODE_INPUT`; `hall_sensor.c` used `HAL_GPIO_ReadPin`.
* Fix: moved to EXTI (rising+falling edge) on PB6/PB7/PB8 with TIM2
  timestamps. `gpio.c` now configures `GPIO_MODE_IT_RISING_FALLING`
  with pull-up. `HAL_GPIO_EXTI_Callback` in `hall_sensor.c` calls
  `HallSensor_Update()` on every Hall edge, giving immediate response
  even when the main loop is busy. EXTI priority is 6 (lower than
  USART/DMA at 5). Polling in `App_Loop` is kept as a backup.
* Acceptance: EXTI fires on every Hall transition; manual wheel turn
  increments edge counter; `.ioc` reflects EXTI mode.

---

## ISSUE-008 — `rpm <nonzero>` does not start PHASE_RUNNING

* Severity: Critical
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`
* Description: `rpm 10` set the SpeedPI target and direction but never
  set `run_request` or `phase = PHASE_RUNNING`, so `service_motor()`
  never reached `MotorDriver_ApplyStep`.
* Evidence: rpm handler set target/direction but not `run_request`;
  `service_motor` run-request branch only handled `MODE_DUTY`.
* Fix: rpm handler sets `run_request = true` (unless a neutral switch
  is started); `service_motor` run-request branch now resolves
  direction for both duty and speed modes and enters `PHASE_RUNNING`.
* Acceptance: after `mode speed` then `rpm 10`, `service_motor` calls
  `MotorDriver_ApplyStep` with the SpeedPI-computed duty.

---

## ISSUE-009 — Startup Hall freshness fault fires too early; `has_ever_run` never set

* Severity: Critical
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`
* Description: Hall freshness was checked immediately on enter to
  `PHASE_RUNNING`. Before the first edge, freshness is stale, and
  `has_ever_run` was never set true, so the `!has_ever_run` branch
  faulted instantly — the motor never had time to start.
* Evidence: `has_ever_run` had no write site; freshness STALE →
  immediate `FAULT_NO_HALL`.
* Fix: `has_ever_run` is set true on the first Hall edge during
  running (edge-counter diff). Before `has_ever_run`, fault only after
  `START_NO_HALL_TIMEOUT_MS` (700 ms) elapsed since `phase_start_ms`.
* Acceptance: motor is allowed 700 ms to produce its first edge;
  `FAULT_NO_HALL` raised only after the timeout.

---

## ISSUE-010 — SpeedPI fault not integrated with FaultManager

* Severity: High
* Status: **FIXED (code)**
* Affected: `App/Src/speed_pi.c`, `App/Src/app_main.c`
* Description: SpeedPI could set `SPD_FAULT_NO_HALL` but the app never
  inspected it, so the motor could keep running and telemetry `FC`
  did not reflect the speed fault.
* Evidence: no caller of `SpeedPI_GetFault()`.
* Fix: `service_motor` checks `SpeedPI_GetFault()`; on
  `SPD_FAULT_NO_HALL` it raises `FaultManager_Raise(FAULT_NO_HALL)`,
  calls `stop_immediate()`, and prints an error. Telemetry `FC` now
  reflects it.
* Acceptance: `spstat` and telemetry `FC` show the speed fault.

---

## ISSUE-011 — Flash storage uses 128 KB stack buffer

* Severity: Critical
* Status: **FIXED (code)**
* Affected: `App/Src/storage/storage.c`
* Description: `Storage_SaveHallMap` / `Storage_SaveConfig` allocated
  `uint8_t sector_buf[128 * 1024]` on the stack. Default stack is
  1 KB — a guaranteed stack overflow.
* Evidence: `uint8_t sector_buf[FLASH_SECTOR_SIZE];` with
  `FLASH_SECTOR_SIZE = 128*1024`.
* Fix: Replaced with append-only record scheme. No large buffer.
  Config records (80 bytes) are written word-by-word directly to flash.
  Hall map (16 bytes) is written similarly. When the config area is
  full, the sector is erased, hall map is preserved by rewriting, and
  a new config record is placed at the start. `savecfg`/`save`/`saveall`
  now work correctly with motor-stopped guard. `map save` is also
  enabled. CRC32 (FNV-1a) validation on every record.
  See `docs/F411_FLASH_CONFIG_PERSISTENCE.md`.
* Acceptance: no large stack allocation remains; `savecfg` and
  `map save` work; flash persists across power-cycle.

---

## ISSUE-012 — Telemetry `PWM_ACT` truncates CCR ticks to uint8_t

* Severity: Medium
* Status: **FIXED (code)**
* Affected: `App/Src/telemetry.c`, `App/Src/motor_driver.c`
* Description: `MotorDriver_GetCurrentCcrTicks()` returns 0..ARR
  (0..6399), but telemetry cast it to `uint8_t`, so `PWM_ACT` wrapped
  and was meaningless.
* Evidence: `uint8_t act = (uint8_t)MotorDriver_GetCurrentCcrTicks();`.
* Fix: added `MotorDriver_GetDuty()` (0..255). Telemetry `PWM_ACT` =
  `MotorDriver_GetDuty()`; `PWM_SET` = `App_GetTargetDuty()`. CCR
  ticks are still available via `MotorDriver_GetCurrentCcrTicks()`.
* Acceptance: `PWM_SET` and `PWM_ACT` are meaningful 0..4000 values
  for `f446_motor_gui.py`.

---

## ISSUE-013 — UART TX is polling / blocking

* Severity: Medium
* Status: **FIXED (code)** — not hardware-verified
* Affected: `App/Src/uart_protocol.c`, `Core/Src/usart.c`,
  `Core/Src/stm32f4xx_it.c`
* Description: `UartProtocol_Print` wrote bytes by polling `TXE`.
  Long telemetry at high frequency could delay Hall polling (now
  mitigated by EXTI on Hall pins — see ISSUE-007).
* Evidence: `while ((huart2.Instance->SR & USART_SR_TXE) == 0U)` loop.
* Fix: replaced polling TX with DMA TX ring buffer. `UartProtocol_Print`
  writes bytes into a 512-byte TX ring buffer and kicks DMA1 Stream6
  Channel 4 (USART2_TX). `HAL_UART_TxCpltCallback` starts the next
  chunk when the DMA transfer completes. The main loop never blocks on
  TX. USART2_IRQHandler handles TC flag for DMA TX completion.
* Acceptance: no blocking TX; DMA transfers are non-blocking; issue
  closed.

---

## ISSUE-014 — `.pio` build artifacts included

* Severity: Low
* Status: **FIXED (code)** — re-fixed in this revision
* Affected: repository root
* Description: `.pio/` build output, `__pycache__/`, and compiled
  binaries were still present in the repository.
* Fix: removed all `.pio/`, `__pycache__/`, `.vscode/` directories
  and `*.elf`, `*.bin`, `*.o`, `*.a`, `*.map` files. Updated
  `.gitignore` to cover all artifact patterns.
* Acceptance: `find . -type d -name ".pio"` returns nothing.

---

## ISSUE-015 — README mismatch

* Severity: Medium
* Status: **FIXED**
* Affected: `README.md`
* Description: README claimed 15 kHz PWM while config was 7.5 kHz
  (center-aligned), described TIM4 Hall interface inconsistently, and
  listed commands as TODO that are implemented.
* Fix: README rewritten to match the code: edge-aligned 15 kHz, TIM2
  micros timestamp, break disabled, save disabled, exact telemetry
  format, bring-up warning.
* Acceptance: README matches actual code and `.ioc`.

---

## ISSUE-016 — `.ioc` not fully CubeMX-consistent with hand-edited code

* Severity: Low
* Status: **FIXED (code)** — CubeMX regeneration not tested
* Affected: `f411-motor-cube.ioc`
* Description: The `.ioc` was hand-maintained. TIM2 was added by hand
  (CubeMX IP list / pin matrix not fully regenerated). If the `.ioc`
  is re-generated in CubeMX, it could overwrite the hand-edited
  `tim.c` (edge-aligned, break disabled, TIM2 micros).
* Fix: updated `.ioc` with TIM2 in the IP list (Prescaler 95,
  Period 0xFFFFFFFF), EXTI configuration for PB6/PB7/PB8
  (GPIO_MODE_IT_RISING_FALLING, pull-up), DMA1_Stream6 NVIC entry,
  and EXTI9_5 NVIC entry. Pin signals updated to GPXTI6/7/8.
  ISSUE-D further cleaned: removed TIM6 (F411 has no TIM6/TIM7),
  removed TIM6_DAC_IRQn, removed invalid F411 RCC fields
  (Ethernet, LCDTFT, PLLSAI2/3, HCLK3/4, RGMIIClk, QSPI, SAI,
  SPDIFRX, SDIO, USB, VCO3).
* Acceptance: `.ioc` matches the active code configuration; build
  passes.

---

## ISSUE-017 — No current sense / current limiting (by design)

* Severity: Info (by-design scope limit)
* Status: **WONTFIX** for this revision
* Affected: whole firmware
* Description: There is no current sense, ADC current measurement, or
  current limiting. This is explicitly out of scope for first
  bring-up. Bench testing must use a current-limited PSU.
* Acceptance: documented in `docs/SAFETY.md` and `docs/KNOWN_RISKS.md`.

---

## ISSUE-018 — Dead-time not bench-verified

* Severity: High
* Status: **OPEN**
* Affected: `Core/Src/tim.c` (DTG=63)
* Description: Dead-time DTG=63 (~0.66 µs @ 96 MHz) is a software
  estimate, not scope-measured against the actual gate driver.
* Fix plan: measure dead-time with a scope on CHx/CHxN during Phase 2
  and adjust DTG if needed.
* Acceptance: dead-time confirmed on scope; no shoot-through observed.

---

## ISSUE-019 — HallSensor_Update called from both EXTI ISR and main loop

* Severity: Critical
* Status: **FIXED (code)** — not hardware-verified
* Affected: `App/Src/hall_sensor.c`, `App/Src/app_main.c`
* Description: `HallSensor_Update()` was called from both
  `HAL_GPIO_EXTI_Callback()` (ISR context) and `App_Loop()` (main
  loop). This modifies shared state (`s_hall.rawCandidate`,
  `s_hall.stableRaw`, `s_hall.hallPeriodUs`, `s_edge_counter`) from
  two contexts without synchronization — a data-race / reentrancy
  bug.
* Fix: single-writer architecture. EXTI ISR only sets a volatile
  pending flag (`s_hall_irq_pending`) and increments an IRQ counter.
  `HallSensor_Update()` checks the flag and runs the debounce state
  machine from main loop only. All ISR-shared variables are volatile.
* Acceptance: Hall state machine has exactly one writer context;
  edge counter increments reliably on manual Hall transitions.

---

## ISSUE-020 — Speed mode command watchdog / host disconnect weak

* Severity: Critical
* Status: **FIXED (code)** — not hardware-verified
* Affected: `App/Src/app_main.c`
* Description: `service_motor()` internally refreshed
  `s_app.last_motor_cmd_ms` while speed mode was running, so a
  single `rpm 23` could keep the motor running indefinitely.
  `service_watchdogs()` exempted speed mode from host disconnect
  detection, so if H7/terminal disconnected, F411 kept driving.
* Fix: removed internal `last_motor_cmd_ms` refresh in speed mode.
  Both duty and speed modes now require real command heartbeat
  (`rpm <signed>` or duty command). Host disconnect detection
  applies to all modes. `mode speed` alone does not run the motor.
* Acceptance: speed mode requires real command refresh; if H7 stops
  sending commands, F411 stops/coasts within `CMD_WATCHDOG_MS`.

---

## ISSUE-021 — Fault restart policy unclear

* Severity: Critical
* Status: **FIXED (code)** — not hardware-verified
* Affected: `App/Src/app_main.c`
* Description: After a fault, new RPM/duty commands could restart
  the motor without explicit `clrerr`. `stop_immediate()` called
  `SpeedPI_Reset()` but not `SpeedPI_Disable()`, and no
  `motion_allowed()` check blocked motion commands during a fault.
* Fix: `stop_immediate()` now calls `SpeedPI_Disable()` (full
  disable). Added `motion_allowed()` helper that checks
  `FaultManager_GetLast() == FAULT_NONE`. All motion commands
  (`f`, `b`, `f<n>`, `b<n>`, `rpm <signed>`, `pwm <n>`, `identify`,
  `scan`, `test`, `gatetest`) are blocked when a fault is latched.
  `clrerr` clears the fault but does not restart motion.
* Acceptance: faulted firmware cannot restart motor until `clrerr`;
  `clrerr` does not start motor; telemetry `FC` shows latched fault.

---

## ISSUE-022 — Float printf unsupported on newlib-nano

* Severity: Medium
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`, `App/Src/uart_protocol.c`
* Description: Code used `%f` / `%.3f` printf format which requires
  `-Wl,-u,_printf_float` on newlib-nano (default for STM32Cube).
  Without the flag, `%f` produces no output or `???`.
* Fix: converted all float printf to scaled integers:
  `Kp=0.600` → `Kp_m=600`, `Ki=0.003` → `Ki_m=3`,
  `Ramp up=30` → `Ramp up=30` (already integer-range).
  `UartProtocol_PrintFloat()` retained for API compatibility but
  internally uses scaled integer.
* Acceptance: `pi`, `kp`, `ki`, `ramp` responses are readable on
  target without float printf linker flag.

---

## ISSUE-023 — No safe single-sector gate test command

* Severity: Medium
* Status: **FIXED (code)** — not hardware-verified
* Affected: `App/Src/app_main.c`
* Description: The `test` command cycles all 6 sectors at duty 60
  for 2 seconds each. No way to test a single sector at low duty
  for scope verification.
* Fix: added `gatetest <sector> <duty>` command. Applies one sector
  at specified duty (1..100) with 500 ms timeout, then allOff.
  Only works when stopped and no fault. Motor-disconnected-only.
* Acceptance: `gatetest 0 20` applies sector 0 at duty 20 for 500 ms,
  then outputs off. Documented in help and BRINGUP.md.

---

## ISSUE-024 — `service_motor()` overrides gatetest/test/identify outputs

* Severity: Critical
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`
* Description: `App_Loop()` calls `ServiceTask_Update()` and
  `service_motor()` back-to-back. When the motor is stopped
  (`PHASE_STOPPED`), `service_motor()` unconditionally calls
  `MotorDriver_AllOff()` in its `else` branch. This immediately
  extinguishes the gate outputs that `ServiceTask_Update()`
  (test/identify) or the `gatetest` handler just applied, so none
  of the bring-up scope commands actually produce a measurable
  output. BRINGUP Stage 2 depends on `gatetest`/`test`.
* Evidence: `service_motor()` final `else { MotorDriver_AllOff(); }`
  runs every loop iteration while `phase == PHASE_STOPPED`;
  `ServiceTask_Update()` and `gatetest` set outputs earlier in the
  same loop pass.
* Fix: `service_motor()` early-returns when `s_app.gatetest_active`
  is true or `ServiceTask_IsActive()` is true, leaving the
  service/gate-test outputs untouched.
* Acceptance: `gatetest 0 20` holds sector 0 outputs for the full
  500 ms timeout; `test` cycles all 6 sectors; `identify` toggles
  produce visible gate drive on a scope.

---

## ISSUE-025 — `stop_immediate()` does not clear `last_motor_cmd_ms`

* Severity: Critical
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`
* Description: `stop_immediate()` cleared `phase`, `direction`,
  duties and `run_request` but left `last_motor_cmd_ms` at its last
  value. `rpm 0` and the startup Hall-timeout path both call
  `stop_immediate()`, so `service_watchdogs()` would later see a
  stale `last_motor_cmd_ms` and raise `FAULT_WATCHDOG` up to 800 ms
  after a legitimate stop. `stop` and `enter_brake()` both clear
  the timestamp — `stop_immediate()` was inconsistent.
* Evidence: `stop_immediate()` had no write to `last_motor_cmd_ms`;
  `service_watchdogs()` returns early only when it is `0U`.
* Fix: `stop_immediate()` now sets `s_app.last_motor_cmd_ms = 0U`.
* Acceptance: after `rpm 0` or a startup-fault stop, no spurious
  `FAULT_WATCHDOG` appears in telemetry `FC`.

---

## ISSUE-026 — `has_ever_run` never reset between runs

* Severity: Critical
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`
* Description: `has_ever_run` is set `true` the first time a Hall
  edge is observed while `PHASE_RUNNING`, but it was never reset to
  `false` when the motor stopped. On a second start with no Hall
  feedback, the 700 ms startup timeout (`START_NO_HALL_TIMEOUT_MS`)
  was bypassed because `has_ever_run` was still `true` from the
  previous run. In duty mode this meant **no Hall-loss timeout at
  all**; in speed mode the 5000 ms `RPM_FEEDBACK_TIMEOUT_MS` applied
  instead of the intended 700 ms startup window.
* Evidence: `has_ever_run` had no reset site; only the
  `!has_ever_run` branch enforces the startup timeout.
* Fix: `stop_immediate()` now clears `has_ever_run` and snapshots
  `last_edge_count` to the current edge counter so the next start
  begins from a known baseline.
* Acceptance: each new run gets the full 700 ms startup window;
  a second start with no Hall edges faults within
  `START_NO_HALL_TIMEOUT_MS`.

---

## ISSUE-027 — DMA RX buffer too small; long bursts lose bytes

* Severity: Critical
* Status: **FIXED (code)**
* Affected: `App/Src/uart_protocol.c`
* Description: `DMA_RX_BUF_LEN` was 32 bytes and the DMA RX ring
  was drained only on the USART IDLE interrupt (no half-transfer or
  transfer-complete callbacks). A burst longer than 32 bytes
  without an idle gap would wrap the DMA buffer and overwrite
  unread bytes. At exactly 32 bytes the NDTR reload made
  `s_dma_last_pos == pos == 0`, so the drain loop copied nothing
  and the whole burst was lost. `UART_LINE_MAX` is 64, so 33+ byte
  commands (e.g. long `boost`/`base`/`pi` lines from H7) could be
  silently truncated or dropped.
* Evidence: `#define DMA_RX_BUF_LEN 32U`; only `App_Usart2RxIsr`
  drained the buffer; no `HAL_UART_RxHalfCpltCallback` /
  `HAL_UART_RxCpltCallback`.
* Fix: `DMA_RX_BUF_LEN` raised to 128. Added
  `HAL_UART_RxHalfCpltCallback` and `HAL_UART_RxCpltCallback` that
  call `App_Usart2RxIsr(0)` at the half and full DMA marks so the
  ring is drained mid-burst even without an idle gap.
* Acceptance: a 64-byte command line is received complete; no byte
  loss at 115200 baud with back-to-back frames.

---

## ISSUE-028 — ApplyStep glitches unchanged phases on every sector change

* Severity: High
* Status: **FIXED (code)**
* Affected: `App/Src/motor_driver.c`
* Description: `MotorDriver_ApplyStep()` always called
  `phase_low_on(new_low)` and `phase_high_pwm(new_high, ...)` even
  when only one of the two phases changed between adjacent sectors.
  The unchanged phase was disabled (CCxE/CCxNE cleared) and
  immediately re-enabled, producing a brief gate glitch and torque
  ripple on every commutation. The legacy Arduino firmware guarded
  these with `if (oldH != newH)` / `if (oldL != newL)`.
* Evidence: `phase_low_on(new_low); phase_high_pwm(new_high, ...)`
  ran unconditionally after the `new_high == s_last_high_idx &&
  new_low == s_last_low_idx` early-return.
* Fix: `ApplyStep` only touches the phase that actually changes:
  `phase_low_on` is called only when `new_low != s_last_low_idx`,
  `phase_high_pwm` only when `new_high != s_last_high_idx`. When
  the high phase is unchanged, the CCR is refreshed directly via
  `*s_phase[new_high].ccr = s_ccr_ticks` so duty updates still
  apply without a disable/enable cycle.
* Acceptance: on a sector transition where only the high phase
  changes, the low-side gate stays continuously ON with no glitch;
  scope shows no spurious edge on the unchanged phase.

---

## ISSUE-029 — `.ioc` OSSI/OSSR mismatch with `tim.c`

* Severity: Medium
* Status: **FIXED (code)**
* Affected: `f411-motor-cube.ioc`, `docs/TIM1_GATE_DRIVE.md`
* Description: The `.ioc` set `TIM1.OffStateIDLE=ENABLE` and
  `TIM1.OffStateRun=ENABLE` but `tim.c` initialised
  `bd.OffStateIDLEMode = TIM_OSSI_DISABLE` and
  `bd.OffStateRunMode = TIM_OSSR_DISABLE`. CubeMX regeneration
  would produce code inconsistent with the hand-edited `tim.c`.
  Additionally `docs/TIM1_GATE_DRIVE.md` claimed disabled channels
  "go to their idle (low) level" while OSSR=0, which per RM0090
  actually makes disabled channels go OFF (Hi-Z), not idle-low.
* Evidence: `.ioc` `OffStateIDLE=ENABLE`/`OffStateRun=ENABLE` vs
  `tim.c` `TIM_OSSI_DISABLE`/`TIM_OSSR_DISABLE`.
* Fix: aligned the `.ioc` to `TIM1.OffStateIDLE=DISABLE` /
  `TIM1.OffStateRun=DISABLE` to match the active code (conservative
  for first bring-up, no behavioural change). Updated
  `docs/TIM1_GATE_DRIVE.md` to accurately describe the RM0090
  truth table: with OSSR=0 disabled channels are OFF (Hi-Z), and
  the `OCxIDLE=RESET` idle level only applies when OSSR=1. Opened
  ISSUE-034 for the future safety improvement of enabling OSSI/OSSR
  so disabled channels are driven to their idle (low) level instead
  of floating.
* Acceptance: `.ioc` and `tim.c` agree; docs match RM0090; build
  passes; no behavioural change to the hot path.

---

## ISSUE-030 — `.ioc` contains invalid F411 `PLLR` field

* Severity: Low
* Status: **FIXED (code)**
* Affected: `f411-motor-cube.ioc`
* Description: `RCC.PLLR=2` was present in the `.ioc`
  `RCC.IPParameters` list and value table. The STM32F411 has no
  PLLR output (only F410/F412/F413/F423). `main.c` already
  documents this in a comment, but the `.ioc` field survived the
  ISSUE-D cleanup.
* Evidence: `.ioc` `RCC.PLLR=2` and `PLLR` in `RCC.IPParameters`.
* Fix: removed `PLLR` from `RCC.IPParameters` and the `RCC.PLLR=2`
  line.
* Acceptance: `.ioc` parses cleanly; no F411-invalid PLLR field;
  build passes.

---

## ISSUE-031 — TIM1 break NVIC not enabled in code

* Severity: Low
* Status: **FIXED (code)**
* Affected: `Core/Src/stm32f4xx_it.c`, `Core/Src/tim.c`
* Description: `TIM1_BRK_TIM9_IRQHandler` and
  `TIM1_UP_TIM10_IRQHandler` are defined in `stm32f4xx_it.c` and
  the `.ioc` lists both IRQs as enabled, but no code ever called
  `HAL_NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn)` /
  `HAL_NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn)`. With break disabled
  this is currently harmless, but if break is ever re-enabled
  (ISSUE-005) the break fault path (`App_Tim1BrkIsr` →
  `FaultManager_Raise(FAULT_HW_BREAK)`) would never fire because
  the NVIC line is masked.
* Evidence: no `HAL_NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn)` anywhere;
  handler defined but unreachable.
* Fix: `MX_TIM1_Init()` now enables both TIM1 break and update
  NVIC lines at priority 5 (matching the `.ioc`), so the break
  fault path is live if break is wired later. No behavioural
  change while break remains disabled (no break events generated).
* Acceptance: `TIM1_BRK_TIM9_IRQn` enabled in NVIC; a future
  break trip reaches `App_Tim1BrkIsr`.

---

## ISSUE-032 — `strtof` reliability on newlib-nano unverified

* Severity: Low
* Status: **OPEN**
* Affected: `App/Src/app_main.c`
* Description: `pi <kp> <ki>`, `ramp <up> <down>`, `kp`, `ki` parse
  float arguments with `strtof`. newlib-nano's `strtof` is not
  guaranteed to be full-precision on all toolchain versions. The
  build links, but the actual on-target parse behaviour for
  `pi 0.6 0.0` etc. has not been hardware-verified. If `strtof`
  returns 0 or a wrong value, the PI gains would silently be wrong.
* Fix plan: verify on hardware that `pi 0.6 0.0` reports
  `Kp_m=600 Ki_m=0`. If it does not, replace `strtof` with a
  scaled-integer parser (e.g. parse `"0.6"` as `600` milli-units).
* Acceptance: `pi`, `kp`, `ki`, `ramp` report correct scaled-integer
  values on target.

---

## ISSUE-033 — `volatile` cast-away on DMA TX buffer pointer

* Severity: Low
* Status: **WONTFIX** (practically safe on F411)
* Affected: `App/Src/uart_protocol.c`
* Description: `s_tx_ring` is declared `volatile uint8_t[]` but
  `HAL_UART_Transmit_DMA(&huart2, (uint8_t *)&s_tx_ring[...], len)`
  casts away `volatile` when passing the buffer to the DMA driver.
  Technically undefined behaviour, but the STM32F411 has no D-cache
  and the producer (CPU) only writes while interrupts are disabled
  in `UartProtocol_Print`, so there is no coherency or race issue
  in practice.
* Fix plan: none for this revision. A clean fix would declare
  `s_tx_ring` as non-`volatile` (the CPU/DMA access is already
  serialised by the primask critical section) and remove the cast.
* Acceptance: documented; no action required for bring-up.

---

## ISSUE-034 — OSSI/OSSR disabled; disabled channels float (Hi-Z)

* Severity: Medium
* Status: **OPEN**
* Affected: `Core/Src/tim.c`, `f411-motor-cube.ioc`
* Description: With `OSSI=0` / `OSSR=0` (ISSUE-029), TIM1 channels
  that are disabled (CCxE=CCxNE=0) go to the OFF state (Hi-Z)
  rather than being driven to their idle level. The gate driver
  inputs therefore float while a phase is disabled. `OCxIDLE=RESET`
  is set but only takes effect when OSSI/OSSR=1. A floating gate
  driver input can be noise-coupled into a partial turn-on.
* Fix plan: after Phase 2 scope verification with the current
  (OSSR=0) configuration, evaluate enabling OSSI=1 / OSSR=1 so
  disabled channels are actively driven to their idle (low) level.
  This is a BDTR change, not a hot-path change, but it alters the
  off-state behaviour and must be scope-verified. Update
  `docs/TIM1_GATE_DRIVE.md` and the `.ioc` together.
* Acceptance: scope shows disabled-phase gate pins held LOW (not
  floating) during the full PWM cycle.

---

## ISSUE-035 — Hall debounce gated by EXTI pending flag

* Severity: Critical
* Status: **FIXED (code)**
* Affected: `App/Src/hall_sensor.c`
* Description: `HallSensor_Update()` only ran the debounce state
  machine when `s_hall_irq_pending` was set by the EXTI ISR. With
  `HALL_STABLE_SAMPLES=2` the first sample set `rawCandidateCount=1`
  and the function returned; the second sample required a NEW EXTI
  event. If that second event never came (motor coasting, edge
  missed, ISR latency) the stable transition was never recognised,
  so `stableRaw`, `lastValidState`, the edge counter and RPM never
  updated. At low RPM this made the motor stall or fault falsely.
* Evidence: `if (!s_hall_irq_pending) return;` at the top of
  `HallSensor_Update()`; `HALL_STABLE_SAMPLES=2` requires two
  consecutive equal samples.
* Fix: removed the pending-flag gate. `HallSensor_Update()` now
  reads the Hall pins and runs the debounce every call (App_Loop
  calls it every iteration). The EXTI ISR still captures a raw
  snapshot + TIM2 timestamp + sequence counter for diagnostics and
  a future ISR-driven RPM path, but the debounce is no longer
  dependent on ISR delivery. Also added `HallSensor_GetFault()` /
  `HallSensor_ClearFault()` and invalid-Hall / illegal-transition
  fault flags (ISSUE-039).
* Acceptance: manual Hall turn reliably increments the edge counter
  and updates `stableRaw` even if EXTI latency is high; RPM reads
  sane at low speed.

---

## ISSUE-036 — `f` / `b` bare commands hard-coded duty=100

* Severity: High
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`
* Description: The `f` / `forward` and `b` / `backward` commands
  set `target_duty = 100U` regardless of `s_app.default_pwm`. The
  legacy Arduino firmware used `controlPwmValue` (default 150) for
  the bare `f`/`b` commands, so the cube firmware was weaker than
  the legacy firmware and `defpwm <n>` had no effect on `f`/`b`.
* Evidence: `s_app.target_duty = 100U;` in both the `f` and `b`
  handlers.
* Fix: `f` / `b` now use `s_app.default_pwm` (default 150, set via
  `defpwm <n>`).  `f<n>` / `b<n>` unchanged (explicit duty, clamped
  0..250).
* Acceptance: `defpwm 180` then `f` drives at duty 180; `f20`
  still drives at duty 20.

---

## ISSUE-037 — Legacy `mode normal` / `mode control` not accepted

* Severity: Medium
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`, legacy Python tools (removed)
* Description: The legacy Arduino firmware used `mode normal`
  (manual PWM) and `mode control` (WASD+RPM). The cube firmware
  only accepted `mode duty` / `mode speed`, so old tools
  scripts that sent `mode normal` / `mode control` got
  `[ERR] Unknown command`.
* Evidence: Legacy `tools/ftdi_h7_client.py` `safe_stop()` sent
  `mode normal`; `tools/ftdi_h7_gui.py` mode buttons sent
  `mode normal` / `mode control`. (Both removed.)
* Fix: the cube parser now accepts `mode normal` as an alias for
  `mode duty` and `mode control` as an alias for `mode speed`.
  The tools scripts were updated to send the canonical
  `mode duty` / `mode speed` names. Both names work; new code
  should use the canonical names.
* Acceptance: `mode normal` and `mode control` no longer return
  `[ERR] Unknown command`; `mode duty` / `mode speed` still work.

---

## ISSUE-038 — Kick/ramp config commands had no effect on duty mode

* Severity: Medium
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`
* Description: `kick_enabled`, `ramp_enabled`, `kick_duty`,
  `kick_ms`, `ramp_step`, `ramp_interval_ms` were stored and
  configurable via `kick on/off`, `ramp on/off`, `kickduty`,
  `kickms`, `ramprate`, `rampms`, but `service_motor()` ignored
  them — it applied `target_duty` directly. The legacy Arduino
  firmware used a kick (high starting duty for `kick_ms`) followed
  by a ramp down to the target duty.
* Evidence: `service_motor()` `duty_update_request` handler set
  `current_duty = target_duty` directly; no kick/ramp logic.
* Fix: added a kick/ramp state machine to `service_motor()` for
  duty mode. On a fresh run request with `kick_enabled=true`, the
  kick duty is applied for `kick_ms`, then the duty ramps toward
  `target_duty` in `ramp_step` increments every `ramp_interval_ms`.
  `kick_enabled=false` skips the kick; `ramp_enabled=false` applies
  the target duty immediately. Numeric config commands now
  range-check their arguments (no uint8_t wrap on negative input).
* Acceptance: `kick on; f50` applies `kick_duty` for `kick_ms`
  then ramps to 50; `kick off; ramp off; f50` applies 50 directly.

---

## ISSUE-039 — Invalid Hall / illegal transition not latching faults

* Severity: High
* Status: **FIXED (code)**
* Affected: `App/Src/hall_sensor.c`, `App/Src/app_main.c`
* Description: `hall_sensor.c` tracked `invalidSinceUs` and
  `invalidTransitionCount` but never raised a fault. The app
  layer only checked Hall freshness (NO_HALL) and SpeedPI feedback
  timeout. A Hall sensor stuck at 0b000 or 0b111, or a noisy Hall
  producing many illegal transitions, would not stop the motor.
* Evidence: no caller of `FaultManager_Raise(FAULT_INVALID_HALL)`
  or `FaultManager_Raise(FAULT_ILLEGAL_TRANSITION)`.
* Fix: `hall_sensor.c` now evaluates `HallFault` flags:
  `HALL_FAULT_INVALID_PERSIST` when an invalid code (0b000/0b111)
  is held longer than `INVALID_HALL_STOP_US` (100 ms), and
  `HALL_FAULT_ILLEGAL_TRANSITION` when
  `invalidTransitionCount > INVALID_TRANSITION_THRESHOLD` (50).
  `service_motor()` checks `HallSensor_GetFault()` while running
  and raises `FAULT_INVALID_HALL` / `FAULT_ILLEGAL_TRANSITION`
  via `FaultManager_Raise`, then `stop_immediate()`. `clrerr`
  also calls `HallSensor_ClearFault()`. No current sense is
  involved — this is purely Hall-pattern based.
* Acceptance: Hall disconnected (stuck 0b000/0b111) for >100 ms
  while running latches `FAULT_INVALID_HALL`; noisy Hall producing
  >50 illegal transitions latches `FAULT_ILLEGAL_TRANSITION`;
  motor stops and `clrerr` is required before new motion.

---

## ISSUE-040 — Speed PI defaults too conservative to start motor

* Severity: High
* Status: **FIXED (code)**
* Affected: `App/Inc/app_config.h`, `App/Src/speed_pi.c`
* Description: The first cube revision set `DEFAULT_BASE_PWM_LOW=20`,
  `DEFAULT_BOOST_LOW_PWM=35`, `SPEED_PI_MAX_PWM=100`, `Kp=0.6`,
  `Ki=0.0`. A low-RPM hub motor could not produce enough torque to
  start with these values — the motor would stutter or not move.
  The legacy Arduino firmware used base=55, boost=65, max=180,
  Kp=0.8, Ki=0.1. Also `SetBasePwm` / `SetBoostPwm` did not clamp
  to `SPEED_PI_MAX_PWM`, so a stray `base` command could push the
  feed-forward above the PI saturation limit.
* Evidence: `app_config.h` values; `SpeedPI_SetBasePwm` /
  `SetBoostPwm` had no clamp.
* Fix: defaults raised to a middle ground (base low=40, boost
  low=55, max=180, Kp=0.8, Ki=0.05, ramp up=60, down=150, boost
  time=150 ms) — strong enough to start the motor, still safe for
  a current-limited bench PSU and no current sense. `SetBasePwm`
  and `SetBoostPwm` now clamp each band to `SPEED_PI_MAX_PWM`.
  Tune via `pi`, `base`, `boost` after first motion.
* Acceptance: `rpm 30` with default params produces enough torque
  to start an unloaded hub motor; `base 999` is clamped to 180.

---

## ISSUE-041 — TIM1 PWM frequency 15 kHz, should be 20 kHz

* Severity: Medium
* Status: **FIXED (code)**
* Affected: `Core/Src/tim.c`, `App/Inc/app_config.h`,
  `f411-motor-cube.ioc`, `docs/TIM1_GATE_DRIVE.md`,
  `App/Src/hall_sensor.c`, `README.md`
* Description: TIM1 PWM was 15 kHz (ARR=6399, 96 MHz / 6400). The
  spec requires 20 kHz. The dead-time comment claimed OSSR/OSSI
  keep disabled channels at idle-low, which is incorrect with
  OSSR=0 (they go Hi-Z — ISSUE-029/034).
* Evidence: `htim1.Init.Period = 6399`, `PWM_PERIOD_TICKS = 6399`,
  docs said 15 kHz.
* Fix: ARR changed to 4799 → 96 MHz / 4800 = 20.0 kHz.
  `PWM_PERIOD_TICKS` updated to 4799. `.ioc` `TIM1.Period=4799`
  added. Dead-time DTG=63 unchanged (clock unchanged, same
  ~0.66 µs); comment corrected to explain DTG<=127 → DT=DTG*t_DTS.
  docs/README/AGENTS updated. Center-aligned would give 10 kHz.
* Acceptance: scope shows 20 kHz PWM on high-side pins;
  `PWM_PERIOD_TICKS` matches ARR.

---

## ISSUE-042 — Parser accepts negative / out-of-range values without reject

* Severity: Medium
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`, `App/Inc/app_config.h`
* Description: Numeric command arguments were parsed with `strtol`
  and cast directly to `uint8_t` / `uint16_t`, so negative or huge
  values wrapped silently. `base -1`, `boost 999`, `kickduty 999`,
  `defpwm 999`, `ramprate -5` were accepted and produced garbage
  config. `rpm 99999` was accepted and commanded a dangerous speed.
* Evidence: `(uint8_t)(v & 0xFF)` in kick/ramp config handlers;
  no range check in `base`/`boost`/`rpm`.
* Fix: all numeric command handlers now range-check and clamp:
  duty/PWM values 0..250, `ms` 0..1000, kick/ramp config has
  explicit max constants (`KICK_DUTY_MAX`, `KICK_MS_MAX`,
  `RAMP_STEP_MAX`, `RAMP_INTERVAL_MS_MAX`, `DEFAULT_PWM_MAX`),
  `rpm <signed>` clamped to ±`MAX_RPM_TARGET` (500). `f<n>`/`b<n>`
  reject trailing garbage. `base`/`boost` use end-pointer validation
  so missing arguments produce `[ERR] Usage:` instead of silently
  using 0.
* Acceptance: `base -1` → Base L=0; `defpwm 999` → DefaultPWM=250;
  `rpm 99999` → RPM=500; `kickduty -5` → KickDuty=0;
  `boost 10 20` (missing arg) → `[ERR] Usage:`.

---

## ISSUE-043 — `pwm <n>` while stopped armed the command watchdog

* Severity: High
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`
* Description: `pwm <n>` always set `s_app.last_motor_cmd_ms =
  HAL_GetTick()`, even when the motor was stopped.  The command
  watchdog (`service_watchdogs`) then started a 800 ms countdown
  from that timestamp.  If no motion command arrived within 800 ms
  the firmware raised `FAULT_WATCHDOG` even though the motor was
  never running — `pwm <n>` while stopped is a legitimate "set the
  target duty for the next run" operation, not a motion command.
  The same applied to `defpwm <n>` and other config commands that
  refreshed the watchdog timestamp.
* Evidence: `pwm <n>` handler did `s_app.last_motor_cmd_ms =
  HAL_GetTick();` unconditionally; `service_watchdogs()` checked
  `last_motor_cmd_ms` regardless of motor phase.
* Fix:
  1. `service_watchdogs()` now early-returns unless the motor is
     `PHASE_RUNNING` or `PHASE_NEUTRAL`.  A stopped / braked /
     faulted motor cannot time out.
  2. `pwm <n>` only refreshes `last_motor_cmd_ms` when the motor is
     already `RUNNING` or `NEUTRAL`.  When stopped, it just records
     `target_duty` for the next `f`/`b`/`f<n>`.
  3. The `duty_update_request` handler in `service_motor()` only
     applies the kick/ramp when the motor is `RUNNING`/`NEUTRAL`;
     when stopped it leaves the outputs off.  The kick/ramp state
     machine is now started from the `run_request` handler when the
     motor transitions `STOPPED -> RUNNING`, so a `pwm <n>` while
     stopped does not arm the kick pulse prematurely.
* Acceptance: `pwm 50` while stopped does not raise
  `FAULT_WATCHDOG` after 800 ms; `f20` starts the motor at duty 20
  (no kick by default); `pwm 80` while running refreshes the
  watchdog and changes the duty.

---

## ISSUE-044 — Default kick too aggressive for no-current-sense bring-up

* Severity: High
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`, `docs/BRINGUP.md`
* Description: `App_Init()` set `kick_enabled = true`,
  `kick_duty = 225`, `kick_ms = 50`.  With no hardware current
  measurement, a 225-duty kick pulse on `f10` could exceed the PSU
  current limit and trip the PSU before the ramp-down to 10.  The
  legacy Arduino firmware used these values, but the cube firmware
  is for first bring-up with an unverified power stage.
* Evidence: `App_Init()` `s_app.kick_enabled = true; s_app.kick_duty
  = 225;`; `defaults` command loaded the same.
* Fix: kick is **disabled by default** (`kick_enabled = false`).
  `kick_duty` default lowered to 60 (used only if `kick on` is sent
  explicitly).  `ramp_step` lowered to 8 with `ramp_interval_ms = 5`
  for a gentle ramp.  `default_pwm` lowered to 100 (safe for bare
  `f`/`b`).  `defaults` command loads the same safe values.
  `docs/BRINGUP.md` Stage 4 now explicitly says to `kick off` before
  the `f10`/`f15`/`f20` tests so they test the actual low duty, and
  documents that kick may be enabled later with a low `kickduty`.
* Acceptance: `f10` with default config ramps gently to duty 10
  (no 225 kick); PSU current stays within limit; `kick on` then
  `f10` applies a 60-duty kick for 50 ms then ramps to 10.

---

## ISSUE-045 — Repeated commands reset startup timeout and kick

* Severity: High
* Status: **FIXED (code)**
* Affected: `App/Src/app_main.c`, `App/Src/hall_sensor.c`
* Description: While the motor was running, receiving the same motion
  command again (e.g. `f20` while already running forward, or
  `rpm 10` while already in speed mode) caused `run_request = true`.
  The `service_motor` handler treated this as a fresh start: it
  reset `phase_start_ms` and restarted the kick pulse. Resetting
  `phase_start_ms` meant a continuous stream of commands (heartbeats)
  would infinitely delay the `START_NO_HALL_TIMEOUT_MS` check,
  effectively disabling the startup Hall-loss fault. Restarting
  the kick pulse caused torque spikes on every heartbeat.
* Evidence: `f` / `b` / `rpm` handlers unconditionally set
  `run_request = true`; `service_motor` unconditionally stamped
  `phase_start_ms = HAL_GetTick()` and entered the kick branch
  when `run_request` was true.
* Fix:
  1. The command parsers (`f`/`b`/`rpm`) now distinguish new
     motion from heartbeats. If the motor is already running in
     the same direction with the same target, they just refresh
     `last_motor_cmd_ms` and return (no `run_request`). If the
     target duty changes mid-run, they update `target_duty` without
     setting `run_request`.
  2. `service_motor()` `run_request` handler only arms the kick
     pulse and stamps `phase_start_ms` on a genuine
     `STOPPED`/`BRAKE` → `RUNNING` transition.
  3. Also protected the Hall EXTI snapshot in `hall_sensor.c` with a
     critical section (`irq_snapshot`) so `seq`, `raw`, and `us`
     cannot tear between reads.
* Acceptance: spamming `f20` while running does not restart the
  kick pulse; spamming `f20` with no Hall sensors connected still
  faults after exactly 700 ms; `f30` while running at `f20`
  smoothly ramps to 30 without a kick.

---

## ISSUE-046 — Remove fault latch and enable active brake

* Severity: High
* Status: **FIXED (code)** — not hardware-verified
* Affected: `App/Src/motion/motion_safety.c`,
  `App/Src/motion/motion_control.c`, `App/Inc/motion/motion_control.h`,
  `App/Src/command/command_handlers_motion.c`,
  `App/Src/command/command_handlers_service.c`,
  `App/Src/command/command_handlers_fault.c`,
  `docs/PROTOCOL.md`, `docs/SAFETY.md`, `docs/BRINGUP.md`,
  `docs/KNOWN_RISKS.md`, `AGENTS.md`
* Description: The user requested that faults stop the motor but do
  not latch, so a new motion command resumes without `clrerr`. The
  user also requested that `brake`/`x` perform active braking (all
  low-side MOSFETs ON) instead of coasting. The previous design
  latched all faults and required `clrerr`, and active braking was
  disabled because there is no current sense.
* Fix:
  * `MotionControl_Allowed()` now always returns `true`.
  * The central fault guard in `MotionControl_Service()` stops the
    motor but no longer transitions to `PHASE_FAULT`.
  * Every motion command handler clears the displayed fault and
    releases the motor-driver safety lock before executing.
  * Added `MotionControl_RequestBrake()` and wired `brake`/`x` to it.
  * Changed the `PHASE_BRAKE` output path to call
    `MotorDriver_ActiveBrake()`.
  * Updated fault/active-brake documentation.
* Acceptance: A watchdog/host-lost/estop fault stops the motor and
  the next `f`/`b`/`rpm`/`pwm` command resumes without `clrerr`;
  `brake`/`x` produces active braking and automatically releases to
  coast after `brake_hold_ms`; build passes.
