# ROADMAP — BLDC Motor Driver Module

> **Project:** STM32 BLDC Motor Driver (Black Pill F411CE)
> **Current State:** Working prototype with WASD Python host (legacy)
> **Target State:** 4-motor skid-steer vehicle with hub STM32

---

## Phase 0: Stabilize Current Code

**Objective:** Fix critical bugs in existing codebase before any protocol changes.

**Tasks:**
- [ ] Fix command queue bottleneck: change `if` to `while` in `processQueuedCommands()` and `processPythonQueuedCommands()`
- [ ] Test software dead-time in `applyDriveState()` — measure if shoot-through occurs
- [ ] Fix `saveall` to include `saveModeToStorage()`
- [ ] Increase `IDENTIFY_TOGGLE_MS` to 50-100ms (from 1ms)
- [ ] Add default PWM to EEPROM config (make `pythonPwmValue` configurable)
- [ ] Unify telemetry "PWM" field semantics or use distinct field names

**Dependencies:** None — current codebase only

**Risks:**
- Dead-time test may reveal hardware issues (MOSFET damage risk)
- Identify step timing may need hardware-specific tuning

**Success Criteria:**
- Command queue processes all queued commands per loop iteration
- Motor commutation works reliably after identify
- Configuration is persistent across reboots

**Test Items:**
- [ ] Rapid command input → all commands processed without delay
- [ ] Identify → produces valid hall map consistently
- [ ] `saveall` → mode persists after reboot
- [ ] Config change → PWM default persists after reboot
- [ ] Telemetry fields are consistent across modes

---

## Phase 1: f/b/s Protocol Implementation

**Objective:** Replace WASD protocol with f/b/s motion protocol in firmware.

**Tasks:**
- [ ] Implement f/b/s command parsing in `processCommand()`:
  - `f` → forward at default PWM (configurable, default 150)
  - `f<duty>` → forward at specified duty (0-255)
  - `b` → backward at default PWM
  - `b<duty>` → backward at specified duty
  - `s` → stop
- [ ] Update `processPythonCommand()` to handle f/b/s (remove w/s/x/d/a)
- [ ] Implement lease-based motion:
  - Each f/b/s command refreshes `lastMotorCommandMs`
  - Enable `checkCommandWatchdog()` in Python mode
  - Motor stops if no motion command for 800ms
- [ ] Update telemetry to use consistent "PWM" field
- [ ] Remove WASD-specific code from firmware (d/a for PWM, w/s for direction)

**Dependencies:** Phase 0 completed

**Risks:**
- Protocol change may break existing Python host
- Watchdog activation in Python mode may cause unexpected stops if heartbeat timing is off

**Success Criteria:**
- `f150` → motor runs forward at PWM 150
- `b100` → motor runs backward at PWM 100
- `s` → motor stops
- No command for 800ms → motor auto-stops
- Telemetry fields are consistent

**Test Items:**
- [ ] Send `f150` → motor runs forward at ~59% duty
- [ ] Send `b100` → motor runs backward at ~39% duty
- [ ] Send `s` → motor stops
- [ ] Send `f150` then wait 1 second → motor auto-stops at 800ms
- [ ] Send `f150` every 600ms → motor continues running (lease renewal)
- [ ] Send `f` → motor runs at default PWM (configurable)
- [ ] Rapid f/b/s commands → all processed correctly

---

## Phase 2: FTDI Connection Test

**Objective:** Verify single motor driver works correctly over FTDI serial connection.

**Tasks:**
- [ ] Connect motor driver to host via FTDI USB-serial adapter
- [ ] Test basic f/b/s commands via serial terminal (minicom, screen, or PuTTY)
- [ ] Verify telemetry reception
- [ ] Test watchdog behavior (send f, then stop sending → motor should stop at 800ms)
- [ ] Test identify command over FTDI
- [ ] Measure round-trip latency (command → telemetry response)
- [ ] Test at different baud rates if needed

**Dependencies:** Phase 1 completed

**Risks:**
- FTDI adapter may have different timing than direct USB
- Serial line noise may cause command parsing errors

**Success Criteria:**
- All f/b/s commands work over FTDI
- Telemetry is received correctly
- Watchdog triggers as expected
- No ghost commands or stale data

**Test Items:**
- [ ] Basic f/b/s via serial terminal
- [ ] Watchdog test: send f, disconnect → motor stops
- [ ] Identify test: hall map produced correctly
- [ ] Long-running test: motor runs for 5 minutes without issues
- [ ] Noise test: add noise to serial line → commands still work

---

## Phase 3: Python Host Update

**Objective:** Rewrite Python host to use f/b/s protocol with lease-based heartbeat.

**Tasks:**
- [ ] Rewrite `wasd_controller.py` or create new `motor_controller.py`:
  - Send `f<default_pwm>` on W key press
  - Send `b<default_pwm>` on S key press
  - Send `s` on key release
  - Send `d` / `a` for PWM adjust (send updated f/b with new duty)
  - Heartbeat: send current command every 600ms while key held
- [ ] Default PWM: configurable, stored in config, default 150
- [ ] Update telemetry parser for consistent field names
- [ ] Remove WASD-specific command mapping
- [ ] Add multi-motor support structure (prepare for Phase 6)

**Dependencies:** Phase 2 completed

**Risks:**
- Keyboard input timing may not match 600ms heartbeat target
- Curses nodelay mode may still have issues with key detection

**Success Criteria:**
- W key → motor runs forward, continues while held
- S key → motor runs backward, continues while held
- Key release → motor stops within 800ms (via watchdog)
- Heartbeat sends command every ~600ms
- PWM adjust works during motor run

**Test Items:**
- [ ] Hold W → motor runs forward continuously
- [ ] Release W → motor stops within 800ms
- [ ] Hold S → motor runs backward continuously
- [ ] Adjust PWM while running → duty changes smoothly
- [ ] Kill Python process → motor stops (watchdog)
- [ ] Disconnect serial cable → motor stops (watchdog)

---

## Phase 4: Safety Layers

**Objective:** Add hardware watchdog and strengthen failsafe mechanisms.

**Tasks:**
- [ ] Add IWDG (Independent Watchdog) initialization in `setup()`:
  - Timeout: 500ms
  - Feed in `loop()` after motor control tick
- [ ] Add host connection monitor:
  - Track last UART activity time
  - If no UART activity for 2 seconds → stop motor
- [ ] Add motor driver self-test on startup:
  - Verify hall sensors respond
  - Verify MOSFET outputs toggle
- [ ] Add emergency stop command (`e` or `estop`) that bypasses all modes
- [ ] Add fault code to telemetry (include `FC:<code>` field)

**Dependencies:** Phase 3 completed

**Risks:**
- IWDG timeout too short may cause resets during normal operation
- Self-test may produce unwanted motor movement

**Success Criteria:**
- Firmware hang → MCU resets within 500ms → motor stops
- Serial disconnect → motor stops within 2 seconds
- Fault codes visible in telemetry
- Emergency stop works from any mode

**Test Items:**
- [ ] Force firmware hang (infinite loop) → MCU resets, motor stops
- [ ] Disconnect serial cable → motor stops within 2s
- [ ] Send `e` → motor stops immediately regardless of mode
- [ ] Trigger fault → fault code appears in telemetry
- [ ] Self-test on startup → reports OK or specific failure

---

## Phase 5: Hub STM32 Integration Preparation

**Objective:** Prepare motor driver module for integration with hub STM32.

**Tasks:**
- [ ] Define motor addressing protocol (e.g., `M1:f150` or separate UART ports)
- [ ] Test multiple motor drivers on separate UART ports (using USB-serial adapters)
- [ ] Ensure motor driver module is self-contained (no dependencies on specific hub)
- [ ] Document hub-to-motor protocol (command format, timing, error handling)
- [ ] Create test harness that simulates hub behavior
- [ ] Verify telemetry format is hub-friendly (easy to parse and forward)

**Dependencies:** Phase 4 completed

**Risks:**
- Multiple UART ports may cause timing conflicts
- Protocol may need adjustment for hub multiplexing

**Success Criteria:**
- 4 motor drivers can be controlled independently via separate UART ports
- Each driver responds with correct telemetry
- No cross-talk between motor drivers
- Hub protocol documentation is complete

**Test Items:**
- [ ] Connect 2+ motor drivers via separate USB-serial adapters
- [ ] Control each independently with f/b/s commands
- [ ] Verify telemetry from each is correctly separated
- [ ] Run all 4 motors simultaneously
- [ ] Measure aggregate telemetry bandwidth

---

## Phase 6: 4-Motor Skid-Steer

**Objective:** Implement full 4-motor skid-steer control via hub STM32.

**Tasks:**
- [ ] Hub STM32 project (separate repository):
  - Receive commands from Python host
  - Route to 4 motor driver UARTs
  - Aggregate telemetry from 4 motors
  - Implement skid-steer logic (optional)
- [ ] Python host for 4 motors:
  - Keyboard: W/S for forward/backward, A/D for turning
  - Or: WASD for skid-steer (left side / right side)
  - Display telemetry for all 4 motors
  - PWM adjust affects all motors
- [ ] Tank steering modes:
  - Arcade: single stick forward/turn
  - Tank: left side / right side independent
- [ ] Safety: all-motor stop on any driver fault

**Dependencies:** Phase 5 completed, hub STM32 project started

**Risks:**
- Hub STM32 development may take significant time
- 4-motor coordination may reveal timing issues
- Power supply requirements for 4 motors

**Success Criteria:**
- 4 motors controlled simultaneously via hub
- Skid-steer turning works correctly
- All motors stop on any fault
- Telemetry from all 4 motors displayed in Python UI

**Test Items:**
- [ ] Forward: all 4 motors run forward
- [ ] Backward: all 4 motors run backward
- [ ] Turn left: right motors forward, left motors backward
- [ ] Turn right: left motors forward, right motors backward
- [ ] Stop: all motors stop
- [ ] Fault on one motor: all motors stop
- [ ] PWM adjust: all motors respond
- [ ] Long-running test: 10-minute drive without issues

---

## Phase Dependency Graph

```
Phase 0 (Stabilize)
    │
    ▼
Phase 1 (f/b/s Protocol)
    │
    ▼
Phase 2 (FTDI Test)
    │
    ▼
Phase 3 (Python Host)
    │
    ▼
Phase 4 (Safety)
    │
    ▼
Phase 5 (Hub Prep)
    │
    ▼
Phase 6 (4-Motor Skid-Steer)
```

---

## Current Status

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 0 | Not started | ISSUES.md documents all issues to fix |
| Phase 1 | Not started | Protocol design in ARCHITECTURE.md |
| Phase 2 | Pending | Depends on Phase 1 |
| Phase 3 | Pending | Depends on Phase 2 |
| Phase 4 | Pending | IWDG, connection monitor |
| Phase 5 | Pending | Hub protocol design |
| Phase 6 | Pending | Requires hub STM32 project |
