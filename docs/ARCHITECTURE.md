# ARCHITECTURE — BLDC Motor Driver Module

> **Version:** 0.2
> **Target MCU:** STM32 Black Pill F411CE
> **Motor Type:** Sensor-based BLDC (3 Hall sensors)
> **Communication:** UART (115200 baud)
> **Architecture:** Timer/event-based async, stop≠brake separation, multi-motor readiness

---

## 1. Project Purpose

This project is a **single-motor BLDC driver module** that:
- Controls one 3-phase BLDC motor via 6-step commutation
- Receives Hall sensor feedback for commutation timing
- Accepts motion commands via UART
- Provides telemetry feedback via UART
- Is designed as a building block for a 4-motor skid-steer vehicle

The module operates independently. Multiple modules connect to a hub STM32 (separate project) which multiplexes commands from a Python host.

---

## 2. Components

### 2.1 Hardware

| Component | Specification | Designator |
|-----------|---------------|------------|
| MCU | STM32F411CE (Black Pill V3) | — |
| Motor | 3-phase BLDC with 3 Hall sensors | — |
| Gate Drivers | 3× L6388ED013TR (half-bridge) | U8, U9, U10 |
| MOSFETs | 6× IRFB7730 N-channel | — |
| Current Sense | 0.5mΩ shunt + INA181A1 (20V/V) | R9, U2 |
| Voltage Sense | Resistive divider (47kΩ / 2.2kΩ) | R12, R13 |
| Gate Resistors | 22Ω per MOSFET | R14–R19 |
| Bootstrap | 1µF cap + diode per phase | C1–C3, D2–D4 |
| Bus Capacitors | 2× 470µF bulk + 2× 100µF support | C4, C5, C21, C22 |
| Regulation | L7805 (5V), Black Pill provides 3.3V | U11 |
| Protection | Fuse + TVS | J3, D1 |
| Hall Conditioning | Pull-up + series network | R1–R6, R10, R11 |
| Throttle Input | Analog filter (47kΩ/47kΩ/22nF) — hardware present | J1, R7, R8, C9 |
| Communication | UART (PA2 TX, PA3 RX) + USB Serial | — |
| Status LED | PC13 (active low) | — |

### 2.2 Pin Map

| Pin | Function | Block |
|-----|----------|-------|
| PB6, PB7, PB8 | Hall sensors (H1, H2, H3) — INPUT_PULLUP | Hall input |
| PA8, PA7 | Phase A high/low (AH, AL) | Gate driver |
| PA9, PB0 | Phase B high/low (BH, BL) | Gate driver |
| PA10, PB1 | Phase C high/low (CH, CL) | Gate driver |
| PA0 | ISENSE — current measurement (INA181A1 output) | Analog |
| PA4 | VSENSE — DC bus voltage measurement | Analog |
| PC13 | Status LED | — |
| PA2, PA3 | UART TX, RX (CMD serial) | Communication |

**Note:** Current sense (PA0) and voltage sense (PA4) are available on hardware but not yet used in firmware. These are planned for Phase 5+ (brake current protection) and future closed-loop control.

### 2.3 Software

| File | Purpose |
|------|---------|
| `src/main.cpp` | Firmware — motor driver, commutation, CLI, telemetry |
| `tools/wasd_controller.py` | Python host — curses-based WASD UI (legacy, being replaced) |

---

## 2.4 Hardware Signal Flow

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  Host/PC    │────▶│  STM32F411  │────▶│ L6388 Gate  │
│  (UART/USB) │     │  (MCU)      │     │   Drivers   │
└─────────────┘     └──────┬──────┘     └──────┬──────┘
                           │                    │
                    ┌──────▼──────┐     ┌──────▼──────┐
                    │ Hall Sensors│     │ 6× IRFB7730 │
                    │ (PB6/7/8)  │     │  MOSFETs    │
                    └──────┬──────┘     └──────┬──────┘
                           │                    │
                    ┌──────▼──────┐     ┌──────▼──────┐
                    │ Current     │     │ Motor Phases│
                    │ Sense (PA0) │     │ A/B/C       │
                    └─────────────┘     └─────────────┘
```

### 2.5 Hardware Components Detail

| Component | Specification | Notes |
|-----------|---------------|-------|
| MCU | STM32F411CE (Black Pill V3) | 100MHz Cortex-M4 |
| Gate Drivers | 3× L6388ED013TR | Half-bridge, bootstrap high-side |
| MOSFETs | 6× IRFB7730 N-channel | High-current capability |
| Current Sense | 0.5mΩ shunt + INA181A1 (20V/V) | PA0 ADC |
| Voltage Sense | Resistive divider (47kΩ/2.2kΩ) | PA4 ADC |
| Gate Resistors | 22Ω per MOSFET | R14-R19 |
| Bus Capacitors | 2×470µF + 2×100µF | DC bus filtering |
| Bootstrap | 1µF caps + diodes per phase | High-side gate drive |
| Protection | Fuse + TVS diode | Input protection |
| Regulation | L7805 → 5V, Black Pill → 3.3V | Power rails |

### 2.6 Pin Map (Complete)

| Pin | Function | Block | Status |
|-----|----------|-------|--------|
| PB6, PB7, PB8 | Hall sensors (H1, H2, H3) | Hall input | Active |
| PA8, PA7 | Phase A high/low (AH, AL) | Gate driver | Active |
| PA9, PB0 | Phase B high/low (BH, BL) | Gate driver | Active |
| PA10, PB1 | Phase C high/low (CH, CL) | Gate driver | Active |
| PA0 | ISENSE (current measurement) | Analog | Hardware ready, firmware TBD |
| PA4 | VSENSE (voltage measurement) | Analog | Hardware ready, firmware TBD |
| PA2, PA3 | UART TX, RX | Communication | Active |
| PC13 | Status LED | Indicator | Active |

### 2.7 Hardware Capabilities vs Firmware Status

| Capability | Hardware | Firmware | Notes |
|------------|----------|----------|-------|
| 6-step commutation | ✅ Ready | ✅ Active | Core function |
| Hall sensing | ✅ Ready | ✅ Active | PB6/7/8 with conditioning |
| PWM drive | ✅ Ready | ✅ Active | 8kHz target, 8-bit |
| Current measurement | ✅ Ready (PA0) | ❌ Not used | INA181A1 + 0.5mΩ shunt |
| Voltage measurement | ✅ Ready (PA4) | ❌ Not used | Resistive divider |
| Brake (low-side short) | ✅ Possible | 🔲 Planned (Phase 4) | All low-side MOSFETs on |
| Dead-time protection | ✅ L6388 internal | ⚠️ Not tested | Gate driver has internal DT |
| Throttle input | ✅ Ready (J1) | ❌ Not used | Analog filter present |

---

## 3. Why Timer/Event-Based Async Architecture?

### 3.1 The Design Choice

Even though the firmware uses asynchronous 6-step commutation (Hall-driven), the **timer-based control scheduler architecture is preserved and protected**. This is not a contradiction — it's a deliberate design decision.

### 3.2 What "Async 6-Step" Means Here

The commutation is asynchronous in the sense that:
- Hall sensor changes trigger state transitions
- The motor doesn't follow a fixed timing pattern
- Commutation adapts to actual motor speed

### 3.3 What "Timer/Event-Based Architecture" Preserves

Despite async commutation, the following timing guarantees are maintained:

| Layer | Timing | Purpose |
|-------|--------|---------|
| PWM generation | Timer-based (hardware PWM peripheral) | Consistent switching frequency |
| Hall sensing | Event-driven (ISR on pin change) | Fast response, low latency |
| Control logic | Scheduler-based (60µs tick) | Predictable execution order |
| UART processing | Main loop (non-blocking) | Doesn't interfere with motor control |

### 3.4 Why This Separation Matters

**Without separation:**
- UART parsing could delay commutation → motor stutters or miscommutates
- Command processing could block the control tick → timing violations
- 4-motor coordination would be impossible → no timing predictability

**With separation:**
- Motor control runs first, guaranteed every 60µs
- UART processing runs after motor control, with bounded execution time
- Each layer has clear timing characteristics
- 4-motor scaling is feasible (each driver has its own scheduler)

### 3.5 Code Evidence

The separation is already implemented in `loop()`:
```cpp
void loop() {
  // 1. Motor control FIRST — highest priority, time-critical
  runMotorControlScheduler();
  
  // 2. UART collection — non-blocking ring buffer fill
  uartDrainToRing(Serial, usbRx);
  uartDrainToRing(CMD, cmdRx);
  
  // 3. Line parsing — bounded execution (64 chars max per call)
  processRxRingToLines(usbRx, usbCli, CommandSource::USB);
  processRxRingToLines(cmdRx, cmdCli, CommandSource::UART);
  
  // 4. Command processing — mode-specific
  // ... (after motor control is done)
}
```

---

## 4. UART Protocol

### 4.1 Motion Commands (f/b/s Protocol)

The module accepts single-character motion commands with optional duty parameter:

| Command | Action | Example |
|---------|--------|---------|
| `f` | Forward at default PWM | `f` |
| `f<duty>` | Forward at specified duty (0-255) | `f150` |
| `b` | Backward at default PWM | `b` |
| `b<duty>` | Backward at specified duty | `b200` |
| `s` | Stop (coast) — all outputs off | `s` |
| `x` | Brake (active) — low-side short | `x` |

**Command parsing:** Commands are terminated by newline (`\n`). Single-character commands use default PWM; numeric suffix sets specific duty.

### 4.2 Lease / Watchdog Semantics

Each motion command (`f`, `b`, `x`) refreshes a timestamp (`lastMotorCommandMs`). If no motion command is received within `CMD_WATCHDOG_MS` (800ms), the motor is automatically stopped (coast).

```
Host:  f150 ──────┬───────┬───────┬───────┬──►
                  │       │       │       │
Motor:  Running   │   Running   │  Stopped │
              800ms     800ms   timeout
```

**Behavior:**
- Forward (`f`) and backward (`b`) commands refresh the lease
- Brake (`x`) refreshes the lease (brake is a controlled stop)
- Stop (`s`) does NOT refresh the lease (immediate coast, no safety timeout needed)
- 800ms without any motion command → automatic coast stop

### 4.3 UART Hot Path Separation

UART processing never directly drives MOSFETs. The data flow is:

```
UART RX → Ring Buffer → Line Parser → Command Queue → Command Processor → pendingReq → Control Tick → MOSFET
```

**Key separation points:**
1. **Ring buffer** absorbs UART data without blocking
2. **Command queue** stores parsed commands without executing them
3. **pendingReq** struct holds intent, not action
4. **Control tick** applies changes atomically

### 4.4 Intent vs Action

| Stage | What Happens | Where |
|-------|--------------|-------|
| UART receives "f150" | Character stored in ring buffer | `uartDrainToRing()` |
| Line complete | "f150" string queued | `enqueueCommand()` |
| Command processed | Intent stored: `hasRunRequest=true, runDirection=1, targetDuty=150` | `processCommand()` |
| Control tick runs | Intent applied: `beginRunRequest()` + `applyDriveState()` | `motorControlTick()` |

The command processor never calls `analogWrite()` or `digitalWrite()` directly. It only sets flags in `pendingReq`. The control tick reads these flags and applies changes.

### 4.5 Telemetry Format

Telemetry is sent every 100ms (configurable) when telemetry is enabled:

```
RPM:<val>,D:<duty>,DIR:<F/R>,PH:<phase>,PWM_SET:<val>,PWM_ACT:<val>,BRAKE:<0/1>,FC:<code>,H:<hall>
```

| Field | Description |
|-------|-------------|
| RPM | Calculated revolutions per minute |
| D | Instantaneous duty cycle (0-255) |
| DIR | Direction: F=forward, R=reverse, S=stopped |
| PH | Motor phase: 0=Stopped, 1=Kick, 2=Running, 3=NeutralWait, 4=Fault, 5=Braking |
| PWM_SET | PWM value set by host (0-255) |
| PWM_ACT | PWM value used by firmware (after ramp/clamp) |
| BRAKE | Brake active flag: 0=off, 1=braking |
| FC | Fault code (0=no fault) |
| H | Raw Hall sensor value (1-6) |

**Example:** `RPM:2450,D:180,DIR:F,PH:2,PWM_SET:200,PWM_ACT:180,BRAKE:0,FC:0,H:3`

---

## 5. Hall ISR Role

### 5.1 Current Implementation

```cpp
void hallISR() {
  isr_rawHall = forceReadRawHall();  // Read 3 pins → 3-bit value
  isr_hallTimeUs = micros();          // Timestamp
  isr_hallEvent = true;               // Set flag
}
```

**Characteristics:**
- **Lightweight:** Only reads 3 pins + sets 2 variables
- **No computation:** No debounce, no validation, no state lookup
- **No MOSFET control:** Never calls analogWrite/digitalWrite
- **Flag-based:** Main loop processes the flag, not the ISR

### 5.2 Why ISR Must Be Light

| Concern | Impact |
|---------|--------|
| ISR latency | If ISR takes too long, motor control tick may be delayed |
| Stacking | If Hall changes faster than ISR can process, events are lost |
| Determinism | ISR should have bounded, predictable execution time |
| Multi-motor | Multiple motors with heavy ISRs could cause CPU starvation |

### 5.3 Main Loop Processing

The main loop reads the ISR flag in `updateHallRuntime()`:
1. Reads `isr_hallEvent` flag (with interrupts disabled)
2. Reads `isr_rawHall` and `isr_hallTimeUs`
3. Clears the flag
4. If no ISR event: reads Hall directly (fallback)
5. Debounces: requires 2 consecutive same readings
6. Validates: checks mapped state, checks transition validity
7. Updates RPM period calculation

This separation ensures the ISR stays light while the main loop handles complex logic.

---

## 6. Control Tick Responsibilities

### 6.1 What the Control Tick Does

```
motorControlTick(nowUs) {
  1. updateHallRuntime()           // Process Hall state (debounce, validate)
  2. applyPendingRequests()        // Apply deferred commands (run, stop, duty)
  3. Phase check:
     - Stopped/Fault: allOff()
     - NeutralWait: check release timer
     - Kick/Running: proceed
  4. Timeout check                 // Startup no-hall fault
  5. Transition spam check         // Illegal hall transitions
  6. Kick phase check              // Kick timeout → running
  7. updateDutyState()             // Ramp duty toward target
  8. Electrical state lookup       // Hall → commutation state
  9. applyDriveState()             // Drive MOSFETs
}
```

### 6.2 What the Control Tick Should NOT Do

| ❌ Should NOT | Why |
|--------------|-----|
| Parse UART | Blocking, unpredictable timing |
| Process commands | May call heavy functions |
| Send telemetry | I/O operation, non-deterministic |
| EEPROM writes | Blocking, long duration |
| Service tasks | Scan/test/identify are slow |

### 6.3 Timing Guarantee

| Parameter | Value |
|-----------|-------|
| Tick period | 60µs (16.6kHz) |
| Max catch-up | 4 ticks (240µs) |
| Expected execution | < 20µs per tick |
| Worst case | ~50µs per tick (with catch-up) |

---

## 7. Stop vs Brake: Why Separate Behaviors?

### 7.1 Current Behavior: Coast Stop

`stopMotorImmediate()` calls `allOff()` which:
- Sets all high-side PWM to 0
- Sets all low-side digital outputs to LOW
- Motor continues spinning due to inertia (coast)
- No current flows through MOSFETs
- **Safe state** — all outputs de-energized

### 7.2 Target Behavior: Brake (Future)

`beginBrake()` will call `brakeAllLowSide()` which:
- Sets all high-side PWM to 0
- Sets all low-side digital outputs to HIGH
- Motor EMF creates current through low-side MOSFETs
- Motor slows down due to dynamic braking
- **Active state** — MOSFETs are conducting

### 7.3 Why They Must Be Separate

| Aspect | Stop (Coast) | Brake (Active) |
|--------|--------------|----------------|
| MOSFET state | All off | Low-side on |
| Current flow | None | Motor EMF through MOSFETs |
| Energy dissipation | Motor inertia only | MOSFETs + motor winding |
| Safety | Safe (de-energized) | Active (current flowing) |
| Suitable for watchdog | ✅ Yes | ❌ No |
| Suitable for fault | ✅ Yes | ❌ No |
| User-controlled | Optional | Must be explicit |

### 7.4 Semantic Difference

- **Stop:** "I want the motor to stop spinning" → Let it coast, it will stop naturally
- **Brake:** "I want the motor to stop NOW" → Actively oppose rotation, controlled deceleration

These are different user intentions and require different implementations.

---

## 8. Why Brake Should NOT Be Fault/Watchdog Default

### 8.1 The Problem with Brake-as-Default

If watchdog timeout triggered brake instead of coast:
- Motor would suddenly brake (unexpected force)
- Current spike through MOSFETs (potential damage)
- User/operator could be surprised (safety hazard)
- In a multi-motor system: one motor braking while others coast = unpredictable behavior

### 8.2 Coast as Default: Why It's Safe

| Scenario | Coast Behavior | Brake Behavior |
|----------|----------------|----------------|
| Watchdog timeout | Motor coasts to stop (gradual) | Motor brakes (abrupt) |
| Host crash | Motor coasts (safe) | Motor brakes (risky) |
| Hall fault | Motor coasts (predictable) | Motor brakes (unpredictable) |
| Power loss | Motor coasts (expected) | Motor brakes (unexpected) |

### 8.3 Design Rule

**Rule:** Fault and watchdog paths ALWAYS lead to coast/all-off. Brake is ONLY available through explicit user command (`k`).

This ensures:
- Default behavior is always safe
- Brake requires conscious user decision
- Watchdog/fault scenarios are predictable
- Multi-motor systems behave consistently

---

## 9. Mevcut Arduino Firmware: What Already Fits

### 9.1 Already-Aligned Structures

| Structure | Status | Evidence |
|-----------|--------|----------|
| Scheduler/control tick | ✅ Aligned | `runMotorControlScheduler()` — 60µs, catch-up |
| Hall cache/ISR | ✅ Aligned | `hallISR()` — lightweight, flag-based |
| UART ring/queue | ✅ Aligned | `RxRing` + `CommandQueue` — non-blocking |
| Software watchdog | ✅ Aligned | `checkCommandWatchdog()` — 800ms |
| Python mode | ✅ Aligned | Separate command parser |
| Deferred apply | ✅ Aligned | `pendingReq` → `applyPendingRequests()` |
| EEPROM persistence | ✅ Aligned | Magic + checksum validation |
| RPM calculation | ✅ Aligned | Hall period-based |

### 9.2 Partially Aligned Structures

| Structure | Status | Issue |
|-----------|--------|-------|
| Command queue processing | ✅ Fixed | `if` → `while` with budget (v0.3.0) |
| Telemetry fields | ✅ Fixed | PWM_SET/PWM_ACT + backward compat (v0.3.0) |
| Default PWM | ✅ Fixed | EEPROM-configurable, default 150 (v0.3.0) |
| Python watchdog | ⚠️ Partial | Infrastructure exists but disabled |

### 9.3 Missing Structures

| Structure | Status | What's Needed |
|-----------|--------|---------------|
| f/b/s protocol | ✅ Implemented | f/b/s with duty suffix, x=brake |
| Lease semantics | ❌ Missing | Timestamp-based motion validation |
| Brake state | ❌ Missing | `MotorPhase::Braking` + `brakeAllLowSide()` |
| Hardware watchdog | ❌ Missing | IWDG initialization and feeding |
| Host connection monitor | ❌ Missing | UART activity tracking |
| Command timestamp | ❌ Missing | Stale command detection |
| Multi-motor abstraction | ❌ Missing | Motor ID in protocol (hub handles this) |

---

## 10. What Needs Refactoring

### 10.1 High Priority Refactors

| Area | Current | Target | Effort |
|------|---------|--------|--------|
| Command queue | ✅ `while` with budget | Drains queue per iteration (v0.3.0) | Done |
| Telemetry | ✅ PWM_SET/PWM_ACT | Unified naming (v0.3.0) | Done |
| Python watchdog | ⚠️ Disabled | Enabled | Low |
| Protocol | WASD (w/s/x/d/a) | f/b/s | Medium |
| Default PWM | ✅ EEPROM-configurable | Default 150 (v0.3.0) | Done |

### 10.2 Medium Priority Refactors

| Area | Current | Target | Effort |
|------|---------|--------|--------|
| Brake state | Missing | `MotorPhase::Braking` | Medium |
| Command timestamp | Missing | Add to `CommandItem` | Low |
| Fault codes | Not in telemetry | Add `FC:<code>` | Low |
| Host connection | Not monitored | Track UART activity | Low |

### 10.3 Low Priority Refactors (Future)

| Area | Current | Target | Effort |
|------|---------|--------|--------|
| Hardware watchdog | Missing | IWDG init + feed | Low |
| Pin configuration | Hardcoded | Config-based | Medium |
| Multi-motor protocol | Single motor | Hub-mediated | High |

---

## 11. Watchdog / Failsafe

### 11.1 Software Watchdog

| Parameter | Value | Description |
|-----------|-------|-------------|
| `CMD_WATCHDOG_MS` | 800 | Timeout for motion command lease |
| `lastMotorCommandMs` | variable | Timestamp of last received motion command |

**Behavior:** If `isMotorDriveActive()` and `(now - lastMotorCommandMs) > CMD_WATCHDOG_MS`, the motor is stopped immediately (coast).

**Active in:** Normal mode, Settings mode
**Inactive in:** Python mode (to be fixed in Phase 2)

### 11.2 Watchdog/Fault Default Behavior Matrix

| Trigger | Default Action | Reason |
|---------|----------------|--------|
| Lease timeout | Coast stop (allOff) | Safe, de-energized |
| Hall timeout | Fault + Coast stop | Motor can't commutate |
| Transition spam | Fault + Coast stop | Hall signal unreliable |
| IWDG reset | MCU reset → allOff | Hardware recovery |
| Host disconnect | Coast stop | No communication |

**Rule:** All fault/watchdog paths lead to coast/all-off. Brake is never triggered automatically.

### 11.3 Hardware Watchdog (Future)

**Status:** NOT IMPLEMENTED

IWDG will be added in Phase 4 with:
- Timeout: 500ms
- Feed point: After `motorControlTick()` in `loop()`
- Behavior: MCU reset on timeout → all outputs off

---

## 12. Command Queue / Buffer Architecture

### 12.1 Data Flow

```
UART RX → RxRing (128 bytes) → Line Parser → CommandQueue (8 items) → Command Processor → pendingReq → Control Tick
```

### 12.2 RxRing

- Size: 128 bytes
- Type: Circular buffer
- Not interrupt-safe (ISR doesn't access it)

### 12.3 Command Queue

- Size: 8 commands
- Processing: ONE per loop iteration (to be changed to `while` loop)
- No timestamps (to be added)

### 12.4 Pending Requests

The `CommandRequest` struct holds deferred commands:
- `hasRunRequest` + `runDirection`
- `hasStopRequest`
- `hasTargetDutyUpdate` + `requestedTargetDuty`
- Future: `hasBrakeRequest`

Applied atomically in `applyPendingRequests()` during the control tick.

---

## 13. Timing Architecture

| Component | Period | Method |
|-----------|--------|--------|
| Motor control tick | 60µs (~16.6kHz) | `runMotorControlScheduler()` with catch-up |
| Telemetry | 100ms | `sendTelemetry()` |
| Watchdog check | Per loop iteration | `checkCommandWatchdog()` |
| Hall debounce | 2 consecutive samples | `updateHallRuntime()` |
| PWM frequency | 8kHz (target) | `analogWriteFrequency()` |
| Ramp update | 10ms (default) | `updateDutyState()` |
| Brake hold | 500ms (default) | `motorControlTick()` (Phase 4) |

---

## 14. Motor State Machine (Current + Planned)

### 14.1 Current States

```
Stopped → (f/b command) → Kick → (kickMs timeout) → Running
Running → (direction change) → NeutralWait → Kick → Running
Any → (stop command / watchdog) → Stopped
Any → (hall fault / transition spam) → Fault
```

### 14.2 Planned States (Phase 4)

```
Running → (brake command k) → Braking → (timeout/release) → Stopped
Braking → (stop command s) → Stopped (immediate coast)
Any → (watchdog/fault) → Stopped (coast, NOT brake)
```

| Phase | Description | Duration |
|-------|-------------|----------|
| Stopped | All outputs off, motor idle | Until command |
| Kick | Initial high-torque burst | `kickMs` (default 120ms) |
| Running | Normal commutation with ramp | Until stop/brake/fault |
| NeutralWait | All off, waiting for current decay | `DIRECTION_NEUTRAL_MS` (80ms) |
| Fault | Error state, outputs off | Until manual reset |
| Braking | Active braking (low-side short) | `brakeHoldMs` (default 500ms) |

---

## 15. EEPROM Storage Layout

| Address | Size | Content |
|---------|------|---------|
| 0 | ~13 bytes | Hall Map (magic, version, map[8], checksum) |
| 64 | ~13 bytes | Config (magic, version, kick/ramp params, checksum) |
| 128 | ~7 bytes | Operating Mode (magic, version, mode, checksum) |

**Planned additions (Phase 1+):**
- `defaultPwm` in Config
- `brakeEnabled` in Config
- `brakeHoldMs` in Config

---

## 16. Safety Layers

### 16.1 Currently Implemented

| Layer | Type | Scope |
|-------|------|-------|
| Software watchdog | `checkCommandWatchdog()` | Normal, Settings modes |
| Hall timeout | `START_NO_HALL_TIMEOUT_MS` | All modes |
| Transition spam | `invalidTransitionCount > 20` | All modes |
| Invalid hall hold | `INVALID_HALL_STOP_US` | All modes |
| Duty clamp | `clampValue(0, 255)` | All modes |
| EEPROM validation | Magic + checksum | Startup |

### 16.2 Planned Safety Layers

| Layer | Phase | Status | Hardware Support |
|-------|-------|--------|------------------|
| Python mode watchdog | Phase 2 | Will be enabled | Software only |
| Hardware watchdog (IWDG) | Phase 4 | Will be added | STM32 peripheral |
| Software dead-time | Phase 1 | Will be tested | L6388 internal DT exists |
| Host connection monitor | Phase 3 | Will be added | Software only |
| Brake timeout | Phase 4 | Will be added | Software + low-side MOSFETs |
| Current-based protection | Phase 5 | Planned | INA181A1 + PA0 ADC (hardware ready) |
| Voltage monitoring | Phase 6 | Planned | Divider + PA4 ADC (hardware ready) |

### 16.3 Hardware Protection Summary

| Protection | Hardware | Notes |
|------------|----------|-------|
| Fuse | J3 | Overcurrent at board input |
| TVS diode | D1 | Transient voltage suppression |
| Gate resistors | 22Ω (R14-R19) | Limits dV/dt, reduces ringing |
| L6388 internal dead-time | Gate driver | Prevents cross-conduction |
| Current sense | INA181A1 + 0.5mΩ shunt | 20V/V gain, PA0 ADC |
| Voltage sense | 47kΩ/2.2kΩ divider | PA4 ADC |

**Note:** L6388 gate drivers include internal dead-time protection, reducing but not eliminating the need for software dead-time. Hardware fuse and TVS provide input-level protection but do not protect against firmware-level overcurrent.

---

## 17. 4-Motor Skid-Steer Architecture (Future)

### 17.1 System Topology

```
┌──────────────┐
│ Python Host  │
│  (USB/UART)  │
└──────┬───────┘
       │ UART (115200)
┌──────▼───────┐
│   Hub STM32  │ (separate project)
│  (multiplexer)│
└──┬───┬───┬───┘
   │   │   │   │ UART (separate ports)
┌──▼┐┌─▼─┐┌▼──┐┌▼──┐
│M1 ││M2 ││M3 ││M4 │ (each running main.cpp)
└───┘└───┘└───┘└───┘
```

### 17.2 Scaling Points

1. **No global state coupling:** Each motor driver is independent
2. **UART-based communication:** Hub addresses each driver separately
3. **Protocol compatibility:** f/b/s is motor-agnostic
4. **Brake modularity:** Brake logic is per-motor, can be coordinated by hub

### 17.3 Skid-Steer Control Logic

| Action | Left Motors | Right Motors |
|--------|-------------|--------------|
| Forward | f\<duty\> | f\<duty\> |
| Backward | b\<duty\> | b\<duty\> |
| Turn Left | b\<duty\> | f\<duty\> |
| Turn Right | f\<duty\> | b\<duty\> |
| Stop (coast) | s | s |
| Stop (brake) | k | k |

---

## 18. Design Decisions and Trade-Offs

### 18.1 Timer-Based Control Tick

**Decision:** 60µs tick instead of event-driven motor control.

**Trade-off:** Higher CPU usage but predictable timing. Essential for multi-motor coordination.

### 18.2 Deferred Command Application

**Decision:** Commands stored in `pendingReq`, applied during control tick.

**Trade-off:** Slight latency (up to 60µs) but atomic state changes. Prevents race conditions.

### 18.3 Coast as Default Stop

**Decision:** Watchdog/fault always trigger coast stop, never brake.

**Trade-off:** Less aggressive stopping but safer. Brake requires explicit user command.

### 18.4 Low-Side Dynamic Brake

**Decision:** First brake implementation uses low-side short, not reverse torque.

**Trade-off:** Less braking force but safer. Reverse torque braking is future consideration.

---

## 19. Known Architectural Risks

| Risk | Category | Impact | Mitigation |
|------|----------|--------|------------|
| No hardware watchdog | Safety | Firmware hang → motor runs | Add IWDG (Phase 4) |
| No Python watchdog | Safety | Host crash → motor runs | Enable watchdog (Phase 2) |
| Single command/iteration | Performance | Queue buildup | Change `if` to `while` (Phase 1) |
| No brake state | Feature | No active braking | Add Braking phase (Phase 4) |
| Hardcoded pins | Flexibility | Board changes need code changes | Config-based pins (future) |
| No motor ID in protocol | Scalability | Hub can't address motors | Hub handles ID (Phase 7) |
| Brake akım riski | Safety | MOSFET damage | Test + timeout (Phase 5) |
