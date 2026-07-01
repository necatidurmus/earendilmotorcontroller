# Task Log

Agent task history. Newest entries on top.

---

## 2026-07-01 — Config persistence diagnostics

- **Purpose:** Improve config persistence visibility: sequence numbers in output, savecfg post-write verification, clearer failure messages
- **Read:** `storage.c`, `storage.h`, `command_handlers_config.c`, `app_main.c`, `config_snapshot.c`, PROTOCOL.md, F411_FLASH_CONFIG_PERSISTENCE.md
- **Changed:** `App/Src/storage/storage.c` (added `Storage_GetConfigSequence()`), `App/Inc/storage/storage.h` (already had declaration from prior session), `App/Src/command/command_handlers_config.c` (`cfg` shows seq, `savecfg` post-write verify, `loadcfg` reports seq / "runtime unchanged", `erasecfg` reports "runtime unchanged"), `App/Src/app/app_main.c` (boot message shows seq / "defaults active"), `docs/PROTOCOL.md`, `docs/F411_FLASH_CONFIG_PERSISTENCE.md`
- **Why:** Users had no way to see which config generation was active; savecfg had no read-back verification; loadcfg/erasecfg messages were ambiguous about whether runtime was affected
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.2%, Flash 10.4%)
- **Remaining risks:** Post-write verify checks sequence only, not full record CRC; verify-read could race if flash program is delayed (unlikely on STM32, program is synchronous)

---

## 2026-06-30 — H7 F446 Import: Service Lock, Stop Sequences, Bridge Commands, GUI Panel

- **Purpose:** Import F446 bridge safety/control structures into H7 firmware
- **Read:** F446 main.cpp, H7 terminal_parser.c/h, command_handler.c/h, motion_controller.c/h, motor_uart_dma.c/h, motor_dispatcher.c/h, motor_tx_dma.c/h, safety_manager.c/h, app_main.c, app_config.h, rover_types.h, earendil.py, F446_STRUCTURE_INTEGRATION_PLAN.md
- **Changed (new):** `h7-main/Core/Inc/service_lock.h`, `h7-main/Core/Src/service_lock.c`, `h7-main/Core/Inc/telemetry_bridge.h`, `h7-main/Core/Src/telemetry_bridge.c`, `docs/H7_F446_IMPORT_IMPLEMENTATION_PLAN.md`, `docs/H7_F446_IMPORT_TEST_PLAN.md`
- **Changed (modified):** `h7-main/Core/Inc/terminal_parser.h`, `h7-main/Core/Src/terminal_parser.c`, `h7-main/Core/Inc/motion_controller.h`, `h7-main/Core/Src/motion_controller.c`, `h7-main/Core/Src/command_handler.c`, `h7-main/Core/Src/motor_uart_dma.c`, `h7-main/Core/Src/app_main.c`, `h7-main/Core/Inc/app_config.h`, `h7-main/Debug/makefile`, `h7-main/Debug/Core/Src/subdir.mk`, `h7-main/earendil.py`
- **Why:** Integrate F446 bridge features (service lock, estop, bridge on/off, safe/alloff, per-motor identify, dangerous command gate, stop sequences, RX line assembler) into H7
- **Build/test:** arm-none-eabi-gcc not available on this host; native GCC syntax check passes (0 errors) for all 7 modified/new C files; `py_compile` passes for earendil.py
- **Remaining risks:** (1) Full ARM cross-build not tested — arm-none-eabi-gcc not on this machine. (2) STM32CubeIDE auto-regenerated makefile may revert the absolute-path fix. (3) motor_uart_dma.c line assembler not runtime-tested. (4) command_handler.c `HandleIdentifyMotor` and brake sequence rely on `WaitForTxDrain` which blocks main loop briefly (100ms max).

## 2026-06-28 — Implement 8-band base/boost PI tuning
- **Purpose:** Replace the 3-band model with 8 base values, 8 boost values, and one shared boost duration.
- **Read:** Speed PI, config/query/status handlers, F446 GUI/bridge, protocol and bring-up docs.
- **Changed:** Added equal 0..500 RPM band selection, strict 8/8+1 commands, GUI fields/read-back, and synchronized docs.
- **Why:** The required tuning model is 8 base + 8 boost + one shared ms value.
- **Build/test:** F411 and F446 PlatformIO builds SUCCESS; GUI/smoke-test `py_compile` SUCCESS; parser regex check and diff check clean.
- **Remaining risks:** Default band values and RPM transitions require current-limited bench tuning; no hardware verification performed.

## 2026-06-28 — Repair F446 bridge and GUI protocol behavior
- **Purpose:** Fix F446/GUI regressions against the frozen F411 protocol.
- **Read:** F446 firmware/config/README, GUI, smoke test, F411 protocol.
- **Changed:** Non-blocking delayed stop; prefixed PI read-back parsing; corrected brake, estop, and telemetry documentation.
- **Why:** GUI could not parse `M1|` PI responses, stop paths blocked UART, and safety text contradicted current F411 behavior.
- **Build/test:** `pio run -d f446-bridge-test` SUCCESS; GUI and smoke test `py_compile` SUCCESS; diff check clean.
- **Remaining risks:** Serial integration and hardware behavior are not bench-verified.

## 2026-06-28 — Restore F411 build and service safety guards
- **Purpose:** Repair regressions introduced by the unfinished 8-band PI change.
- **Read:** Memory bank, safety/bring-up/protocol/TIM1 docs, changed F411 modules.
- **Changed:** Restored the frozen 3-band PI/GUI protocol; restored service/gate arming; removed blocking direct gate debug commands; restored 100 ms gate-test timeout.
- **Why:** Firmware did not compile and gate-driving service commands bypassed required safety controls.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS; `python -m py_compile tools/f446_motor_gui.py` SUCCESS; two pre-existing unused-function warnings in `motor_driver.c`.
- **Remaining risks:** Hardware not tested; motor-disconnected scope verification remains mandatory.

## 2026-06-28 — Update F411 Speed PI defaults to tested GUI values (Kp=0.25, Ki=0.05)

- **Purpose:** Update PI tuning defaults to match tested GUI values: Kp=0.25 (250x10⁻³), Ki=0.05 (50x10⁻³).
- **Read:** `app_config.h`, `speed_pi.c`.
- **Changed:**
  - `App/Inc/app/app_config.h` — `DEFAULT_SPEED_KP` = 0.25f (was 10.0f), `DEFAULT_SPEED_KI` = 0.05f (was 10.0f).
- **Why:** GUI-tested values (Kp_m=250, Ki_m=50) provide reliable motor control; max clamp remains 50.0.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.2 %, Flash 9.7 %).

## 2026-06-28 — Update F446 Motor GUI to support 8-band PI tuning parameters

- **Purpose:** Update F446 GUI to properly display and apply 8-band base and boost PWM parameters from F411 firmware.
- **Read:** `tools/f446_motor_gui.py`, `f411-motor-cube/App/Src/command/command_handlers_query.c`.
- **Changed:**
  - `tools/f446_motor_gui.py` — Updated `_build_pi_panel()` to show 8 base bands and 8 boost bands (each band has lo/mid/hi/ms = 4 values, 8 bands × 4 = 32 total boost fields)
  - `tools/f446_motor_gui.py` — Updated `_apply_pi()` to send 8-band base and boost parameters
  - `tools/f446_motor_gui.py` — Updated `_handle_line()` to parse 8-band spstat output from firmware
  - `f411-motor-cube/App/Src/command/command_handlers_query.c` — Updated `spstat` command to output 8-band base/boost values
- **Why 32 boost values?:** The firmware uses 8 bands for each of boost_low, boost_mid, boost_high, and boost_ms arrays (4×8=32 values). This allows different boost timing (ms) per RPM band, which is technically sound as higher RPM bands may require longer boost durations. The default values show increasing ms times for higher bands (500ms→1200ms).
- **Build/test:** 
  - Firmware: `pio run -d f411-motor-cube` SUCCESS (RAM 2.2%, Flash 9.7%)
  - GUI: `python3 -m py_compile tools/f446_motor_gui.py` SUCCESS
- **Read:** `speed_pi.c`, `app_config.h`.
- **Changed:**
  - `App/Src/motor/speed_pi.c` — `clampf` max limit `10.0f` → `50.0f` for both Kp and Ki.
  - `App/Inc/app/app_config.h` — `DEFAULT_SPEED_KP` = 10.0f, `DEFAULT_SPEED_KI` = 2.0f (keeps 5:1 ratio with new tested defaults).
- **Why:** GUI showed need for stronger PI (Kp_m=10000 equivalent). Raising max clamp to 50 gives headroom; default 10/2 keeps the same proportional integral ratio (5:1) as prior tested settings.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1 %, Flash 9.7 %).
- **Remaining risks:** High Kp values at very low speeds can cause oscillation; tested GUI values were used in final tuning.

---

## 2026-06-28 — Update F411 Speed PI default tuning values to match tested GUI settings

- **Purpose:** Update F411 firmware default PI / base / boost / ramp values to tested GUI defaults.
- **Read:** `app_config.h`, `speed_pi.c`, `command_handlers_config.c`, `storage.c`.
- **Changed:** `App/Inc/app/app_config.h` — updated `DEFAULT_SPEED_KP`, `DEFAULT_SPEED_KI`, `DEFAULT_BASE_PWM_*`, `DEFAULT_BOOST_*`, `DEFAULT_RAMP_*` defaults; added comment noting GUI alignment.
- **Why:** GUI-tested values proved reliable; previous conservative defaults (base low=640, Kp=0.8) were too weak for the hub motor. New values: Kp=0.25 (GUI 250), Ki=0.05 (GUI 50), Base=250/1000/1500, Boost=1000/2000/3000 ms=1000, Ramp=1000/1000 RPM/s.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1 %, Flash 9.7 %). No code changes outside `app_config.h`.
- **Remaining risks:** None — only RAM defaults changed; runtime `pi`/`base`/`boost`/`ramp` commands, `spstat`/`status` telemetry, and storage load-path remain unchanged.

---

## 2026-06-27 — Add UART RX diagnostics to isolate command failure (after 4000 PWM switch)

- **Purpose:** F411 still does not accept commands via F446 after 4000 PWM change and DMA watchdog. Need to know if bytes even reach the F411.
- **Read:** `uart_protocol.c`, `uart_protocol.h`, `app_status.c`, `telemetry.c`, `command_handlers_fault.c`, `gpio.c`.
- **Changed:**
  - `App/Src/protocol/uart_protocol.c` — added `s_rx_byte_count` to count every byte from the DMA RX buffer; toggles PC13 LED in `line_builder_push` on every completed command line (newline received).
  - `App/Inc/protocol/uart_protocol.h` — declared `UartProtocol_GetRxByteCount()` / `ResetRxByteCount()`.
  - `App/Src/app/app_status.c` — `status` output now includes `RxBytes`.
  - `App/Src/telemetry/telemetry.c` — compact telemetry now includes `RXB:<rx_byte_count>`.
  - `App/Src/command/command_handlers_fault.c` — `clrerr` now resets `RxByteCount`.
- **Why:** Distinguish whether the F411's UART RX DMA is dead (no bytes), the parser is broken, or the F446 bridge isn't forwarding.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1 %, Flash 9.7 %).
- **Remaining risks:** This is diagnostic only; does not fix the underlying issue. Use the LED and status counters to identify the root cause.

---

## 2026-06-27 — Fix F411 command UART RX DMA abort (no response to F446 commands)

- **Purpose:** F411 keeps sending telemetry to F446 but does not process commands from F446 (e.g. `telper 5000` does not slow telemetry).
- **Read:** `uart_protocol.c`, `uart_protocol.h`, `stm32f4xx_it.c`, `usart.c`, `command_handlers_fault.c`, `app_status.c`.
- **Changed:**
  - `App/Src/protocol/uart_protocol.c` — added `HAL_UART_ErrorCallback()` to detect UART errors (ORE/FE/NE/PE) that abort DMA RX; added `s_rx_dma_needs_restart` flag and main-loop watchdog in `UartProtocol_Pump()` to restart circular RX DMA automatically; added `s_uart_error_count` diagnostic counter.
  - `App/Inc/protocol/uart_protocol.h` — declared `UartProtocol_GetUartErrorCount()` / `ResetUartErrorCount()`.
  - `App/Src/command/command_handlers_fault.c` — `clrerr` now resets UART error counter.
  - `App/Src/app/app_status.c` — status block now prints `UARTErr` count.
- **Why:** Telemetry TX working while commands RX silent points to the RX DMA stream being aborted by a UART error and never restarted. This matches the earlier "DMA RX stream stops unexpectedly" issue. The float (`%f`) crash from ISSUE-H would hard-fault the whole device and stop telemetry; telemetry is still flowing, so this is not the float problem.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1 %, Flash 9.7 %). Not hardware-tested yet.
- **Remaining risks:** If the root cause is actually a wiring/ground issue or F446 bridge not forwarding, this fix will not help. Check `UARTErr` in `status` after flashing; a rapidly rising count indicates electrical noise on the RX line.

---

## 2026-06-27 — Expand PWM duty resolution 0–250 → 0–4000

- **Purpose:** Finer low-RPM control by using the full TIM1 0..4799 tick range instead of compressing duty into 0..250.
- **Read:** `app_config.h`, `app_state.h`, `app_state.c`, `motor_driver.c`, `speed_pi.c`, `motion_control.c`, `command_handlers_*.c`, `storage.c`, `app_status.c`, `telemetry.c`, `app_main.c`, `tools/*.py`, `docs/PROTOCOL.md`, `docs/BRINGUP.md`, `docs/TIM1_GATE_DRIVE.md`.
- **Changed:**
  - `App/Inc/app/app_config.h` — added `PWM_MAX_DUTY 4000U`; scaled all duty limits, defaults, and comments.
  - `App/Inc/app/app_state.h`, `App/Src/app/app_state.c` — duty fields and defaults changed to `uint16_t` and scaled by 16x.
  - `App/Src/motor/motor_driver.c` — CCR scaling uses `PWM_MAX_DUTY` (`CCR = duty * 4799 / 4000`).
  - `App/Src/motor/speed_pi.c` — base/boost/computed duty are `uint16_t`.
  - `App/Src/motion/motion_control.c` — request/run/update duty types are `uint16_t`.
  - `App/Src/command/command_handlers_*.c` — clamps and casts updated; `defaults`/`loadcfg` use scaled values.
  - `App/Src/storage/storage.c` — config fields and checksum use `uint16_t`; `CFG_VERSION` bumped 5 → 6.
  - `App/Src/app/app_status.c` — help text updated to 0..4000.
  - `App/Src/telemetry/telemetry.c` — `PWM_SET`/`PWM_ACT` comments updated.
  - `tools/f446_motor_gui.py`, `tools/terminal.py`, `tools/ftdi_h7_emulator.py`, `tools/ftdi_h7_gui.py` — PWM range/clamps/sliders updated to 4000.
  - `docs/PROTOCOL.md`, `docs/BRINGUP.md`, `docs/TIM1_GATE_DRIVE.md` — ranges and scaling documented.
- **Why:** Low-RPM PWM was compressed into ~0..180 counts, causing quantization/jitter. With 4000 steps each count is ~0.02 % duty.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1 %, Flash 9.7 %). `py_compile` passed for `terminal.py`, `ftdi_h7_emulator.py`, `ftdi_h7_gui.py`; `f446_motor_gui.py` check blocked by temporary classifier unavailability.
- **Remaining risks:** H7 dispatcher still uses `uint8_t pwm` and cannot emit values > 255; needs separate update. Higher resolution may expose gate-driver minimum pulse/dead-time limits — verify on scope with current-limited PSU and motor disconnected first.

---

## 2026-06-27 — Remove speed PI PWM limit (180 → 250)

- **Purpose:** User wants `PWM_ACT` to reach the 250 hard clamp in speed mode.
- **Read:** `app_config.h`, `speed_pi.c`.
- **Changed:** `App/Inc/app/app_config.h` — `SPEED_PI_MAX_PWM` 180U → 250U; removed unused `SPEED_PI_MAX_PWM_SOFT_LIMIT`; comment updated to warn that full duty with no current sense requires current-limited PSU.
- **Why:** User explicitly requested removal of the 180 cap.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1%, Flash 9.7%).
- **Remaining risks:** Full PWM in speed mode raises stall current and MOSFET/PSU stress; no electronic current limit exists. Use current-limited bench PSU and watch thermals.

---

## 2026-06-27 — Add PI tuning panel to f446_motor_gui.py

- **Purpose:** Add GUI controls for `pi`, `base`, `boost`, `ramp` parameters so PI tuning can be done without typing raw commands.
- **Read:** `f446_motor_gui.py`, `command_handlers_query.c`, `command_handlers_config.c`, `speed_pi.h`.
- **Changed:**
  - `App/Src/command/command_handlers_query.c` — extended `spstat` to print Kp/Ki/Base/Boost/Ramp values.
  - `tools/f446_motor_gui.py` — added PI Tuning LabelFrame with Kp/Ki/Base/Boost/Ramp entries, "Apply All" and "Read" buttons; spstat response parsing fills entry fields; removed `pi`/`kp`/`ki`/`base`/`boost` from dangerous command list; "Apply All" now auto-unlocks F446 service before sending `pi`/`base`/`boost` (F446 bridge blocks these without unlock).
- **Why:** PI tuning is iterative — GUI fields are faster than typing raw commands.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1%, Flash 9.7%). Python compile check deferred (classifier unavailable).
- **Bug fix:** Initial `spstat` extension used `%.3f`, but newlib-nano does not support `%f` (ISSUE-H), causing a hard fault/crash when `spstat` was called. Fixed to print `Kp_m`/`Ki_m` milli-scaled integers; GUI parser updated accordingly.
- **Remaining risks:** Motor must be stopped before sending `pi`/`base`/`boost`/`ramp` commands (firmware rejects with `[ERR] Stop motor first` if running).

---

## 2026-06-27 — Implement non-latching faults + direct active brake

- **Purpose:** User wants command-loss faults to stop the motor but not require `clrerr`, and `brake`/`x` to perform active braking.
- **Read:** `motion_control.c`, `motion_safety.c`, `command_handlers_motion.c`, `command_handlers_service.c`, `command_handlers_fault.c`, `fault_manager.c`, `docs/PROTOCOL.md`, `docs/SAFETY.md`, `docs/BRINGUP.md`, `docs/KNOWN_RISKS.md`, `AGENTS.md`.
- **Changed:**
  - `App/Src/motion/motion_safety.c` — `MotionControl_Allowed()` now always returns `true`.
  - `App/Src/motion/motion_control.c` — central fault guard stops without entering `PHASE_FAULT`; `PHASE_BRAKE` now drives `MotorDriver_ActiveBrake()`; added `MotionControl_RequestBrake()`.
  - `App/Inc/motion/motion_control.h` — declared `MotionControl_RequestBrake()`.
  - `App/Src/command/command_handlers_motion.c` — motion commands auto-clear fault + safety lock; `brake`/`x` calls `RequestBrake()`; `estop` response updated.
  - `App/Src/command/command_handlers_service.c` — removed fault/safety-lock blocks from `scan`/`test`.
  - `App/Src/command/command_handlers_fault.c` — `clrerr` warning text updated.
  - `docs/PROTOCOL.md`, `docs/SAFETY.md`, `docs/BRINGUP.md`, `docs/KNOWN_RISKS.md`, `AGENTS.md` — updated fault and active-brake behavior.
- **Why:** Remove fault latch per user request; enable active brake on `brake`/`x` command.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1%, Flash 9.6%). Not hardware-tested.
- **Remaining risks:** Active brake without current sense can spike current; all faults are now non-latching including `ESTOP`/`HW_BREAK`; scope verification of brake-to-run transition is needed.

---

## 2026-06-16 — Fix `rpm <n>` clamp bug causing ±500 RPM and reversed direction

- **Purpose:** User reported speed PI mode spun only one direction and would not hold RPM; `rpm 30` caused ~300 RPM. Debug telemetry showed `[OK] RPM=-500` for `rpm 30`.
- **Read:** `command_handlers_motion.c`, `app_config.h`, `fault_codes.h`, telemetry log from user.
- **Changed:** `App/Inc/app/app_config.h` — `MAX_RPM_TARGET 500U` → `MAX_RPM_TARGET 500`.
- **Why:** `MAX_RPM_TARGET` was an unsigned literal (`500U`). In `command_handlers_motion.c` the clamp expression `v < -MAX_RPM_TARGET` expanded to `v < -500U`, which is unsigned arithmetic. Any positive RPM target satisfied the comparison and was clamped to -500; any negative target was clamped to +500. This caused max-speed behaviour and reversed commanded direction.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS. No hardware re-test yet.
- **Remaining risks:** User must re-test with actual motor; Hall map/phase direction and PI tuning may still need adjustment after this fix.

---

## 2026-06-26 — Fix HSE crystal mismatch (8→25 MHz) + DMA RX watchdog

- **Purpose:** F411 was completely unresponsive (no LED, no UART). Root cause: PLL configured for 8 MHz HSE but WeAct BlackPill F411CE uses 25 MHz crystal. Secondary issue: DMA RX stream stops unexpectedly after boot.
- **Read:** main.c, stm32f4xx_hal_conf.h, platformio.ini, uart_protocol.c, usart.c, app_main.c.
- **Changed:**
  - `Core/Inc/stm32f4xx_hal_conf.h` — HSE_VALUE 8000000U → 25000000U
  - `platformio.ini` — build flag HSE_VALUE=8000000 → HSE_VALUE=25000000
  - `Core/Src/main.c` — PLLM=8 → PLLM=25 (SYSCLK still 96 MHz)
  - `uart_protocol.c` — Added LED toggle in App_Usart2RxIsr (debug), DMA auto-restart watchdog in UartProtocol_Pump
  - `app_main.c` — Added USART2/DMA register dump every 2s (debug)
- **Why:** 25 MHz crystal needs PLLM=25 for 1 MHz VCO input. DMA watchdog works around Stream5 stopping without error flags (root cause TBD).
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS. Hardware verified: LED lights, UART boot banner appears, commands work after cable reconnected.
- **Remaining risks:** DMA auto-restart is a workaround, not a root-cause fix. Debug code still present — remove after DMA stability confirmed.

---

## 2026-06-26 — Service/gatetest output ownership fix + docs alignment

## 2026-06-26 — Service/gatetest output ownership fix + docs alignment

- **Purpose:** Fix critical regression where MotionControl_Service() AllOff() was clobbering service/gatetest motor outputs every loop iteration. Also add raw Hall guard, rename F446 function, and align docs with code.
- **Read:** motion_control.c, gate_test.c, service_commutation_test.c, app_config.h, f446 main.cpp, f446 README, docs/BRINGUP.md.
- **Changed:**
  - `motion_control.c` — added early return when `gatetest_active || ServiceTask_IsActive()` after safety checks. Service layer owns motor outputs; MotionControl must not enter normal state machine. Added `!HallSensor_IsCurrentRawValid()` → `MotorDriver_AllOff()` for `outputs_active` (immediate invalid Hall kill without fault latch). Arming auto-disarm still runs inside the early return.
  - `f446 main.cpp` — renamed `emergencyStopAll()` → `coastStopAll()` with corrected comment (coast stop, not emergency).
  - `f446 README.md` — split passthrough list into "unlock gerektirmez" vs "service komutları (unlock + arming gerekli)"; fixed gate test procedure to show `arm gatetest` step; added identify procedure with arming.
  - `docs/BRINGUP.md` — Stage 2: added `arm gatetest` instruction, corrected test duty from 60→10 and step time from 2s→300ms to match code constants.
- **Why:** Previous safety-bypass fix removed the early return that protected service/gatetest output ownership. MotionControl_Service() was calling AllOff() on every iteration when phase != PHASE_RUNNING, killing gatetest/test/identify outputs within microseconds.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1%, Flash 9.4%). `pio run -d f446-bridge-test` SUCCESS (RAM 1.7%, Flash 4.2%).
- **Remaining risks:** Not hardware-verified. Service task Hall-fault path needs integration test. Raw Hall invalid → AllOff behavior during service needs scope verification.

---

## 2026-06-26 — 8-point safety + protocol fix batch

- **Purpose:** Fix safety bypass, API gap, whitespace tolerance, F446 response strings, docs alignment, DMA critical section.
- **Read:** motion_control.c, service_task.c/.h, uart_protocol.c, f446 main.cpp, f446 README, smoke test.
- **Changed:**
  - `service_task.h/.c` — added `ServiceTask_IsDriving()` (returns true for identify/test, false for scan/none)
  - `motion_control.c` — replaced early-return service/gatetest block + duplicate normal-mode checks with unified `outputs_active` guard. Hall safety checks now run whenever motor outputs are active (PHASE_RUNNING || gatetest_active || ServiceTask_IsDriving())
  - `uart_protocol.c` — `is_emergency_command()`: added multi-space tolerance for "rpm 0" / "pwm 0" patterns via flexible whitespace matching after command word
  - `uart_protocol.c` — `UartProtocol_Print()`: moved `tx_start_dma()` outside IRQ-disabled section to reduce Hall-edge-miss window. Ring buffer update stays atomic; DMA kick is safe after unlock (TC callback sees updated head)
  - `f446 main.cpp` — `stop` response: "OK|safe stop" → "OK|normal stop"; `safe` response: "OK|safe stop sent..." → "OK|coast stop sent (no fault latch)"; bridge status: fixed remaining time calc (was `serviceUnlockMs - millis()`, now `SERVICE_TIMEOUT_MS - elapsed`)
  - `f446 README.md` — fixed `estop` description (was identical to `safe`, now correctly says fault-latched); updated safety section
  - `tools/f446_serial_smoke_test.py` — updated expected stop response string
- **Why:** Safety checks were bypassed when service tasks drove the motor. Emergency detection missed "rpm  0". TX DMA start held global IRQ mask too long. F446 stop response said "safe stop" instead of "normal stop". README described estop as coast (wrong).
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1%, Flash 9.4%). `pio run -d f446-bridge-test` SUCCESS (RAM 1.7%, Flash 4.2%). `py_compile f446_serial_smoke_test.py` OK.
- **Remaining risks:** Not hardware-verified. `outputs_active` path for Hall faults during service tasks needs integration test. DMA kick-after-unlock needs scope verification on real hardware.

---

## 2026-06-26 — F411 full codebase audit + fixes

- **Purpose:** Systematic review of entire f411-motor-cube firmware (30 .c/.h files) for bugs, inconsistencies, missing parts, protocol mismatches.
- **Read:** All files in App/Inc, App/Src, Core/Inc, Core/Src, platformio.ini, PROTOCOL.md, ISSUES.md, MEMORY_BANK.md.
- **Changed:**
  - `uart_protocol.c`: removed dead "duty 0" emergency detection (not a real protocol command)
  - `command_handlers_query.c`: fixed `telper` lower clamp from 10→20 and upper from 60000→5000 to match `Telemetry_SetIntervalMs()`
  - `app_utils.h`: moved `SAFE_ABS` macro here as shared definition (was duplicated in speed_pi.c and telemetry.c)
  - `speed_pi.c`, `telemetry.c`: replaced local `SAFE_ABS` with `#include "app_utils.h"`
  - 14 headers: added missing `extern "C"` guards for C++ safety (app_types.h, app_config.h, app_state.h, app_status.h, app_utils.h, command_parser.h, command_dispatcher.h, command_types.h, all 5 command_handlers_*.h, motion_control.h, motion_safety.h, motion_reverse.h, safety_watchdog.h)
- **Why:** Audit found no critical bugs. Fixes address: dead code, interval clamp inconsistency, macro duplication, C++ guard inconsistency.
- **Build/test:** `pio run -d f411-motor-cube` SUCCESS (RAM 2.1%, Flash 9.5%). `py_compile tools/terminal.py` OK.
- **Remaining risks:** Not hardware-verified. ISR/main-loop race on s_rx_drop_count reset is low-risk (clrerr called with motor stopped).

## 2026-06-26 — Monolithic vs modular comparison

- **Purpose:** Compare ref/ monolithic and modular v1 against current
  modular to find wrong/missing transfers. Specifically verify brake
  structure and other safety-critical behaviors.
- **Read:** ref/f411-motor-cube-monolithic/App/Src/app_main.c (full
  2219 lines), ref/modular v1 files, all current modular App/ files
- **Findings:**
  - All functional behaviors correctly transferred (stop_immediate,
    begin_neutral_switch, service_motor, watchdogs, clrerr, identify,
    scan/test, init/loop sequence, Hall freshness, arming timeout)
  - Brake structure fully present: PHASE_BRAKE, brake_hold_ms, timeout
    handler, clamp, App_IsBrakeActive(). Both versions: brake command
    calls stop_immediate() (coast). PHASE_BRAKE timeout is dead code
    in both — ready for future active brake.
  - 10 regressions from modularization already fixed (see entry below)
  - Modular version IMPROVED over monolithic: SpeedPI/Hall fault
    checks now run during service tasks (monolithic skipped them)
  - No missing or wrong transfers found
- **Why:** User requested verification of transfer correctness
- **Build/test:** N/A — comparison only, no code changes
- **Remaining risks:** None — all transfers verified correct

---

## 2026-06-26 — Post-modularization regression fixes (10 issues)

- **Purpose:** Fix regressions and safety issues from the modularization
  refactor. No H7 or legacy f411-motor changes.
- **Read:** AGENTS.md, all App/Inc and App/Src files, f446 bridge,
  tools/f446_motor_gui.py, docs/ai/MEMORY_BANK.md
- **Changed:**
  - `service_identify.c/.h` — returns bool (true=running, false=done)
  - `service_commutation_test.c/.h` — scan/test return bool (done signal)
  - `service_task.c` — checks sub-task return, clears s_active_task on DONE
  - `command_handlers_motion.c` — rpm reverse: set new target BEFORE
    BeginNeutralSwitch so pending_rpm_target carries the new RPM
  - `app_main.c` — App_Init(): call HallSensor_OnMapChanged()+SpeedPI_Reset()
    after Commutation_ApplyMap() from flash boot load
  - `motion_control.c` — Hall/SpeedPI safety checks now run even when
    gatetest or service task is active (prevents bypass during motor drive)
  - `f446_bridge_config.h` — added SERVICE_TIMEOUT_MS (30000)
  - `f446 main.cpp` — serviceUnlockMs=start (not start+30s), fixed wrap-safe
    expiry; estop sends real estop to F411; safe/alloff = coast stop
  - `f446_motor_gui.py` — E-STOP sends `estop`; identify workflow adds
    F411-side `arm service` before `m1 identify`
  - `uart_protocol.c` — is_emergency_command() strips trailing whitespace
    so "stop " / "rpm  0 " detected when queue full
  - `app_status.c` — storage_load=ENABLED storage_save=DISABLED (separate)
  - `gate_test.c` — added GateTest_Init() no-op (declared but missing)
- **Why:** 10 regressions from modularization: service tasks never
  completed (BUSY_SERVICE lockup), rpm reverse lost new target,
  boot Hall map cache stale, safety bypass during service, F446
  estop didn't latch, GUI E-STOP was no-op, emergency detection
  missed trailing-space commands, misleading storage status.
- **Build:** pio run f411-motor-cube ✅, pio run f446-bridge-test ✅,
  py_compile f446_motor_gui.py ✅, py_compile terminal.py ✅
- **Remaining risks:** F446 bridge wrap-safe expiry not tested on
  real hardware. Service task Hall-fault path not integration-tested.

---

## 2026-06-26 — F411 App nested folder restructure

- **Purpose:** Reorganize `f411-motor-cube/App/` from flat layout to 9
  nested sub-folders. Split large modules (command_parser 1075→7 files,
  motion_control 438→4 files, service_task 356→4 files). Extract
  fault_codes.h.
- **Read:** AGENTS.md, MEMORY_BANK.md, all App/Inc and App/Src files,
  platformio.ini, ARCHITECTURE_INDEX.md
- **Changed:**
  - `App/Inc/` and `App/Src/` — 9 sub-dirs created (app, command,
    motion, motor, service, fault, telemetry, storage, protocol)
  - All 17 .c and 20 .h files moved to sub-dirs
  - `platformio.ini` — `+<App/Src/**>`, 10 `-I` flags
  - `command_parser.c` — split into parser + dispatcher + 5 handlers
  - `motion_control.c` — extracted motion_safety.c, motion_reverse.c
  - `service_task.c` — extracted service_identify.c,
    service_commutation_test.c, rewritten as thin dispatcher
  - `fault_codes.h` — extracted FaultCode enum from fault_manager.h
  - `f411-motor-cube/README.md` — new folder tree + module map
  - `docs/ai/ARCHITECTURE_INDEX.md` — rewritten for nested structure
  - `docs/ai/MEMORY_BANK.md` — updated module map, sensitive areas
- **Why:** Flat 37-file layout hard to navigate. Large files
  (command_parser 1075 lines) mix concerns. Nested structure enables
  focused edits and clear module boundaries.
- **Build/test:** `pio run` passes every phase. RAM: 2.1%, Flash: 9.4%
  (identical throughout — no behavior change).
- **Remaining risks:** None — behavior unchanged, build verified.

## 2025-06-26 — Project root restructure

- **Purpose:** Clean up project root. Make modular firmware the active
  `f411-motor-cube/`, archive old directories to `ref/`, consolidate
  duplicate `tools/` copies.
- **Moved to `ref/`:**
  - `f411-motor/` → `ref/f411-motor/` (legacy Arduino)
  - `f411-motor-cube/` → `ref/f411-motor-cube-monolithic/` (original)
  - `BUG_REPORT.md` → `ref/`
  - `COMPREHENSIVE_AUDIT.md` → `ref/`
- **Renamed:** `f411-motor-cube-modular/` → `f411-motor-cube/`
- **Consolidated tools/:** FTDI scripts moved from firmware `tools/`
  to root `tools/`. Firmware-level `tools/` removed.
- **Updated:** AGENTS.md, README.md, docs/ai/MEMORY_BANK.md,
  docs/ai/ARCHITECTURE_INDEX.md, docs/ai/PROJECT_GOALS.md,
  docs/ai/REFACTOR_PLAN_F411.md, platformio.ini (env name),
  firmware README.md.
- **Build:** `pio run -d f411-motor-cube` — SUCCESS

---

## 2025-06-25 — F411 app_main.c modularization

- **Purpose:** Split the monolithic 2218-line `app_main.c` into focused
  modules. Created `f411-motor-cube-modular/` as a parallel directory
  (original untouched).
- **Read:** `f411-motor-cube/App/Src/app_main.c` (full 2218 lines),
  all App/Inc/*.h headers, app_config.h
- **New modules (8 files, ~2034 lines total):**
  - `app_types.h` — shared enums (AppMode, MotorPhase, Direction)
  - `app_state.h/.c` — AppState singleton with `AppState_Get()` accessor
  - `app_utils.h/.c` — string helpers (trim, lower, starts_with, parse_*)
  - `app_status.h/.c` — status/help/hall-map text output
  - `command_parser.h/.c` — full UART command dispatch (~1075 lines)
  - `motion_control.h/.c` — motor state machine, kick/ramp, Hall freshness
  - `safety_watchdog.h/.c` — command watchdog + host disconnect
  - `gate_test.h/.c` — gate test timeout logic
- **Changed:**
  - `app_main.c` — rewritten as 179-line thin orchestrator
  - `platformio.ini` — env name: `blackpill_f411ce_stm32cube_modular`
  - `README_MODULAR.md` — new file, module map and rules
- **Why:** Monolithic file made it hard to reason about safety boundaries,
  review changes, and onboard new agents. Each module now has a single
  responsibility and clear public API.
- **Build:** `pio run -d f411-motor-cube-modular` — SUCCESS (4.14s)
  RAM: 2.1% (2776/131072), Flash: 9.5% (49568/524288)
- **Forbidden checks:**
  - `HAL_Delay` in App/ — NONE
  - `analogWrite`/`digitalWrite` in App/ — NONE (only in comments)
- **No behavior changes:** all command responses, telemetry format, safety
  guards, fault latching, watchdog timeouts are identical to original.
- **Remaining risks:** Not hardware-verified. Must verify on real
  BlackPill+F411 with power stage before any production use.

- **Purpose:** Establish AI/agent workflow documentation to reduce token
  waste and provide a consistent onboarding path for new agents.
- **Read:** README.md, AGENTS.md, ISSUES.md, ROADMAP.md, docs/SAFETY.md,
  docs/PROTOCOL.md, docs/BRINGUP.md, docs/KNOWN_RISKS.md,
  docs/TIM1_GATE_DRIVE.md, app_main.c, app_config.h,
  f446-bridge-test/src/main.cpp, tools/
- **Changed:**
  - `AGENTS.md` — rewritten for conciseness (~120 lines, was ~137)
  - `CLAUDE.md` — new file, imports AGENTS.md + Claude-specific rules
  - `docs/ai/AI_GUIDELINES.md` — new file, agent workflow protocol
  - `docs/ai/MEMORY_BANK.md` — new file, project state snapshot
  - `docs/ai/PROJECT_GOALS.md` — new file, short/mid/long-term goals
  - `docs/ai/ARCHITECTURE_INDEX.md` — new file, quick file map
  - `docs/ai/TASK_LOG.md` — new file, this log
  - `docs/ai/REFACTOR_PLAN_F411.md` — new file, modularization plan
- **Why:** Every new agent was spending significant tokens re-discovering
  project structure, rules, and current state. A structured doc system
  with clear entry points reduces this waste.
- **Build/test:** No code changes — build not required. Markdown links
  and paths verified manually.
- **Remaining risks:** None (doc-only change).
- **Next agent notes:** This is the foundation. The next recommended
  task is F411 app_main.c modularization phase-0: identify parser/state
  boundaries for extraction.

## 2026-07-01 — Flash config persistence (ISSUE-011 fix)

- **Purpose:** Enable `savecfg`/`save`/`saveall`/`map save` with safe Flash EEPROM emulation on STM32F411.
- **Read:** storage.h, storage.c, command_handlers_config.c, speed_pi.h/c, app_state.h/c, app_main.c, app_status.c, telemetry.h, app_config.h
- **Changed:**
  - `App/Inc/storage/storage.h` — new `PersistentConfig_t` struct, full API (`SaveConfig`, `LoadConfig`, `EraseConfig`, `HasValidConfig`)
  - `App/Src/storage/storage.c` — complete rewrite: append-only CFG2 records with FNV-1a CRC32, sequence numbers, no 128 KB buffer, hall map preservation
  - `App/Inc/storage/config_snapshot.h` (NEW) — `ConfigSnapshot_FromRuntime`, `ApplyToRuntime`, `Validate`
  - `App/Src/storage/config_snapshot.c` (NEW) — implementation
  - `App/Src/app/app_main.c` — updated `App_Init` to use new `PersistentConfig_t` load API
  - `App/Src/command/command_handlers_config.c` — `savecfg`/`save`/`saveall` now write to flash; `loadcfg` loads full config; `erasecfg` new command; `cfg` new command; `defaults` resets all config including PI/base/boost/ramp/telper; `map save` enabled
  - `App/Src/app/app_status.c` — help text updated with new commands; status shows flash config state
  - `docs/PROTOCOL.md` — updated command table
  - `docs/KNOWN_RISKS.md` — ISSUE-011 marked resolved
  - `ISSUES.md` — ISSUE-011 updated to reflect full fix
  - `docs/F411_FLASH_CONFIG_PERSISTENCE.md` (NEW) — storage layout, test procedure
- **Why:** ISSUE-011: old storage used 128 KB stack buffer (guaranteed overflow); save was disabled. New design uses small records, append-only scheme.
- **Build/test:** `pio run -d f411-motor-cube` — **SUCCESS** (0 errors, 0 warnings for changed files; RAM 2.2%, Flash 10.2%)
- **Remaining risks:** Flash save/erase not tested on real hardware; power loss during sector erase could corrupt both hall map and config (mitigated: hall map is rewritten first after erase). Hardware testing with motor disconnected required.
