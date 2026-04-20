# ARCHITECTURE — BLDC Motor Driver Module

> **Version:** 0.1
> **Target MCU:** STM32 Black Pill F411CE
> **Motor Type:** Sensor-based BLDC (3 Hall sensors)
> **Communication:** UART (115200 baud)

---

## 1. Project Purpose

This project is a **single-motor BLDC driver module** that:
- Controls one 3-phase BLDC motor via 6-step commutation
- Receives Hall sensor feedback for commutation timing
- Accepts motion commands via UART
- Provides telemetry feedback via UART
- Is designed as a building block for a 4-motor skid-steer vehicle

The module operates independently. Multiple modules are connected to a hub STM32 (separate project) which multiplexes commands from a Python host.

---

## 2. Components

### 2.1 Hardware

| Component | Description |
|-----------|-------------|
| MCU | STM32F411CE (Black Pill) |
| Motor | 3-phase BLDC with 3 Hall sensors |
| MOSFET Driver | 6x N-channel MOSFET (AH, AL, BH, BL, CH, CL) |
| Hall Sensors | 3x digital Hall sensors (H1, H2, H3) |
| Communication | UART (PA2 TX, PA3 RX) + USB Serial |
| Status LED | PC13 (active low) |

### 2.2 Pin Map

| Pin | Function |
|-----|----------|
| PB6, PB7, PB8 | Hall sensors (H1, H2, H3) — INPUT_PULLUP |
| PA8, PA7 | Phase A high/low (AH, AL) |
| PA9, PB0 | Phase B high/low (BH, BL) |
| PA10, PB1 | Phase C high/low (CH, CL) |
| PC13 | Status LED |
| PA2, PA3 | UART TX, RX (CMD serial) |

### 2.3 Software

| File | Purpose |
|------|---------|
| `src/main.cpp` | Firmware — motor driver, commutation, CLI, telemetry |
| `tools/wasd_controller.py` | Python host — curses-based WASD UI (legacy, being replaced) |

---

## 3. Firmware Layer

### 3.1 Architecture Overview

```
┌─────────────────────────────────────────────────┐
│                    main.cpp                      │
│                                                  │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐  │
│  │ UART RX  │  │ Command  │  │ Motor Control │  │
│  │ Ring Buf │→│  Queue   │→│  (60µs tick)  │  │
│  └──────────┘  └──────────┘  └───────────────┘  │
│                                                  │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐  │
│  │  Hall    │  │ Power    │  │  Telemetry    │  │
│  │  ISR     │→│  Stage   │  │  (100ms)      │  │
│  └──────────┘  └──────────┘  └───────────────┘  │
│                                                  │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐  │
│  │  EEPROM  │  │ Watchdog │  │  Service      │  │
│  │  Storage │  │ (800ms)  │  │  Tasks        │  │
│  └──────────┘  └──────────┘  └───────────────┘  │
└─────────────────────────────────────────────────┘
```

### 3.2 Execution Flow (loop())

```
loop() {
  1. runMotorControlScheduler()    // 60µs motor tick (highest priority)
  2. uartDrainToRing(USB)          // collect USB serial data
  3. uartDrainToRing(CMD)          // collect UART data
  4. processRxRingToLines(USB)     // parse lines from USB ring buffer
  5. processRxRingToLines(CMD)     // parse lines from UART ring buffer
  6. mode-specific processing:
     - Python mode: processPythonQueuedCommands()
     - Normal/Settings: processQueuedCommands()
  7. updateServiceTask()           // scan/test/identify
  8. sendTelemetry()               // send telemetry (mode-dependent)
  9. checkCommandWatchdog()        // software watchdog (Normal/Settings only)
}
```

### 3.3 Motor Control Tick (60µs)

```
motorControlTick(nowUs) {
  1. updateHallRuntime()           // debounce, validate hall state
  2. applyPendingRequests()        // apply deferred commands
  3. phase check:
     - Stopped/Fault: allOff()
     - NeutralWait: check release timer
     - Kick/Running: proceed
  4. timeout check                 // startup no-hall fault
  5. transition spam check         // illegal hall transitions
  6. kick phase check              // kick timeout → running
  7. updateDutyState()             // ramp duty toward target
  8. electrical state lookup       // hall → commutation state
  9. applyDriveState()             // drive MOSFETs
}
```

### 3.4 Motor State Machine

```
                    ┌───────────┐
                    │  Stopped  │←────────────────────┐
                    └─────┬─────┘                     │
                          │ f/b command                │ stop command
                    ┌─────▼─────┐                     │
                    │   Kick    │ (optional)           │
                    └─────┬─────┘                     │
                          │ kickMs timeout             │
                    ┌─────▼─────┐                     │
                    │  Running  │─────────────────────┤
                    └─────┬─────┘                     │
                          │ direction change           │
                    ┌─────▼─────┐                     │
                    │NeutralWait│                     │
                    └─────┬─────┘                     │
                          │ DIRECTION_NEUTRAL_MS       │
                    ┌─────▼─────┐                     │
                    │   Kick    │ (new direction)      │
                    └───────────┘                     │
                                                      │
                    ┌───────────┐                     │
                    │   Fault   │─────────────────────┘
                    └───────────┘  (allOff)
```

**Phase Descriptions:**

| Phase | Description | Duration |
|-------|-------------|----------|
| Stopped | All MOSFETs off, motor idle | Until command received |
| Kick | Initial high-torque burst | `kickMs` (default 120ms) |
| Running | Normal commutation with ramp | Until stop or fault |
| NeutralWait | All off, waiting for current to decay | `DIRECTION_NEUTRAL_MS` (80ms) |
| Fault | Error state, all outputs off | Until manual reset |

### 3.5 6-Step Commutation Table

| Step | High Side | Low Side | Active Pins |
|------|-----------|----------|-------------|
| 0 | BH | AL | PB9(PWM) + PA7(HIGH) |
| 1 | CH | AL | PA10(PWM) + PA7(HIGH) |
| 2 | CH | BL | PA10(PWM) + PB0(HIGH) |
| 3 | AH | BL | PA8(PWM) + PB0(HIGH) |
| 4 | AH | CL | PA8(PWM) + PB1(HIGH) |
| 5 | BH | CL | PB9(PWM) + PB1(HIGH) |

### 3.6 Hall Sensor Processing

**ISR (`hallISR`):** Reads all 3 Hall pins, stores raw value with microsecond timestamp. Minimal processing — only reads pins and sets a flag.

**Main loop (`updateHallRuntime`):** 
1. Reads ISR flag (or reads directly if no ISR event)
2. Debounces: requires `HALL_STABLE_SAMPLES` (2) consecutive identical readings
3. Validates: checks if raw value maps to a valid commutation state (0-5)
4. Validates transition: ensures the state change is ±1 (no skipping)
5. Calculates RPM period from ISR timestamps

**Hall Map:** Maps raw 3-bit Hall values (1-6) to commutation states (0-5). Stored in EEPROM with magic number and checksum.

---

## 4. Python Host Layer

### 4.1 Current Implementation (Legacy WASD)

The current Python host (`wasd_controller.py`) provides a curses-based terminal UI with WASD keyboard controls:
- W → Forward (hold-to-run)
- S → Backward (hold-to-run)
- D/↑ → PWM increase
- A/↓ → PWM decrease
- X → Stop
- Q → Quit

**Communication:** Sends "w", "s", "x", "d", "a" commands via UART. Receives telemetry via UART.

**Known Issues:**
- Hold-to-run mechanism is broken (see ISSUES.md ISSUE-01 in context of Python side)
- Uses WASD semantic commands that don't match target protocol

### 4.2 Target Python Host (f/b/s Protocol)

The Python host will be rewritten to:
- Send `f<duty>` for forward (e.g., `f150`)
- Send `b<duty>` for backward (e.g., `b150`)
- Send `s` for stop
- Send heartbeat every ~600ms while key is held
- Immediately send `s` when key is released
- Use configurable default duty (target: 150)

---

## 5. UART Protocol

### 5.1 Current Protocol (WASD — Legacy)

| Command | Description | Direction |
|---------|-------------|-----------|
| `w` | Forward (persistent) | Python → Firmware |
| `s` | Backward (persistent) | Python → Firmware |
| `x` | Stop | Python → Firmware |
| `d` | PWM +10 | Python → Firmware |
| `a` | PWM -10 | Python → Firmware |

### 5.2 Target Protocol (f/b/s)

| Command | Description | Direction |
|---------|-------------|-----------|
| `f<duty>` | Forward at duty (0-255) | Host → Firmware |
| `b<duty>` | Backward at duty (0-255) | Host → Firmware |
| `s` | Stop | Host → Firmware |
| `f` | Forward at default duty | Host → Firmware |
| `b` | Backward at default duty | Host → Firmware |

**Lease Semantics:** Each motion command refreshes a timestamp. If no new motion command arrives within `CMD_WATCHDOG_MS` (800ms), the firmware stops the motor automatically.

### 5.3 Telemetry Format

**Python Mode:**
```
RPM:<value>,D:<duty>,DIR:<F/R>,PH:<phase>,PWM:<set>,PDIR:<dir>,H:<hall>
```

**Normal Mode:**
```
RPM:<value>,D:<duty>,DIR:<F/R>,PH:<phase>,PWM:<target>,PDIR:<dir>
```

**Field Descriptions:**

| Field | Python Mode | Normal Mode |
|-------|-------------|-------------|
| RPM | Calculated RPM | Calculated RPM |
| D | `motorRt.currentDuty` | `motorRt.currentDuty` |
| DIR | Direction (F/R) | Direction (F/R) |
| PH | Phase enum (0-4) | Phase enum (0-4) |
| PWM | `pythonPwmValue` (set value) | `motorRt.targetDuty` (firmware target) |
| PDIR | Python direction (1/-1/0) | Firmware direction (1/-1) |
| H | Hall raw value | — |

### 5.4 OK Response Format

```
OK:FWD,PWM:<duty>    — Forward command accepted
OK:REV,PWM:<duty>    — Backward command accepted
OK:STOP              — Stop command accepted
PWM:<duty>           — PWM changed
[MODE] PYTHON        — Mode switch confirmation
[MODE] NORMAL        — Mode switch confirmation
```

### 5.5 Line Termination

- Commands: `\n` (LF) terminated
- Responses: `\r\n` (CRLF) terminated
- Idle timeout: 150ms — incomplete lines are auto-sent after timeout

---

## 6. Watchdog / Failsafe

### 6.1 Software Watchdog

| Parameter | Value | Description |
|-----------|-------|-------------|
| `CMD_WATCHDOG_MS` | 800 | Timeout for motion command lease |
| `lastMotorCommandMs` | variable | Timestamp of last received motion command |

**Behavior:** If `isMotorDriveActive()` and `(now - lastMotorCommandMs) > CMD_WATCHDOG_MS`, the motor is stopped immediately.

**Active in:** Normal mode, Settings mode
**Inactive in:** Python mode (see ISSUES.md ISSUE-03)

### 6.2 Hardware Watchdog

**Status:** NOT IMPLEMENTED

No IWDG (Independent Watchdog) initialization or feeding exists in the code. If the firmware hangs, the motor will continue running at whatever duty was last applied.

### 6.3 Hall Timeout

| Parameter | Value | Description |
|-----------|-------|-------------|
| `START_NO_HALL_TIMEOUT_MS` | 400 | Fault if no valid hall within this time after start |
| `INVALID_HALL_STOP_US` | 25000 | Fault if hall stays invalid for this duration |

### 6.4 Transition Spam Protection

If `hallRt.invalidTransitionCount > 20`, the motor is faulted with `MotorFaultCode::IllegalTransitionSpam`.

---

## 7. Command Queue / Buffer Architecture

### 7.1 Data Flow

```
UART RX → RxRing (128 bytes, circular) → Line Parser → CommandQueue (8 items) → Command Processor
```

### 7.2 RxRing

- Size: 128 bytes
- Type: Circular buffer
- Push: `rxPush()` — called from `uartDrainToRing()`
- Pop: `rxPop()` — called from `processRxRingToLines()`
- Not interrupt-safe (ISR does not access ring buffer)

### 7.3 Line Parser (`processRxRingToLines`)

- Reads characters from ring buffer
- Accumulates into line buffer (64 chars max)
- On `\r` or `\n`: enqueues completed line
- On idle timeout (150ms): enqueues incomplete line
- Budget: max 64 characters per call

### 7.4 Command Queue

- Size: 8 commands
- Item: `CommandItem { char text[64]; CommandSource src; }`
- Enqueue: `enqueueCommand()` — fails if full (`queueOverflowFlag = true`)
- Dequeue: `dequeueCommand()` — fails if empty
- **Processing: ONE command per loop iteration** (see ISSUES.md ISSUE-01)

### 7.5 Pending Requests

A separate `CommandRequest` struct holds deferred commands that are applied in `applyPendingRequests()` during the motor control tick:
- `hasRunRequest` + `runDirection`
- `hasStopRequest`
- `hasTargetDutyUpdate` + `requestedTargetDuty`

This separation ensures motor state changes happen atomically within the control tick, not during UART processing.

---

## 8. Timing Architecture

| Component | Period | Method |
|-----------|--------|--------|
| Motor control tick | 60µs (~16.6kHz) | `runMotorControlScheduler()` with catch-up |
| Telemetry | 100ms | `sendTelemetry()` / `sendPythonTelemetry()` |
| Watchdog check | Per loop iteration | `checkCommandWatchdog()` |
| Hall debounce | 2 consecutive samples | `updateHallRuntime()` |
| PWM frequency | 8kHz (target) | `analogWriteFrequency()` (if supported) |
| Ramp update | 10ms (default) | `updateDutyState()` |

### 8.1 Motor Control Catch-Up

If the loop is delayed and multiple 60µs periods have passed, the scheduler runs up to `MAX_CONTROL_CATCHUP_TICKS` (4) ticks in a single loop iteration. If more than 4 ticks are missed, the scheduler resets and starts fresh.

---

## 9. EEPROM Storage Layout

| Address | Size | Content |
|---------|------|---------|
| 0 | ~13 bytes | Hall Map (magic, version, map[8], checksum) |
| 64 | ~13 bytes | Config (magic, version, kick/ramp params, checksum) |
| 128 | ~7 bytes | Operating Mode (magic, version, mode, checksum) |

Each structure uses magic number validation and XOR checksum for integrity.

---

## 10. Safety Layers

### 10.1 Currently Implemented

| Layer | Type | Scope |
|-------|------|-------|
| Software watchdog | `checkCommandWatchdog()` | Normal, Settings modes |
| Hall timeout | `START_NO_HALL_TIMEOUT_MS` | All modes |
| Transition spam | `invalidTransitionCount > 20` | All modes |
| Invalid hall hold | `INVALID_HALL_STOP_US` | All modes |
| Duty clamp | `clampValue(0, 255)` | All modes |
| EEPROM validation | Magic + checksum | Startup |

### 10.2 Missing Safety Layers

| Layer | Risk | Status |
|-------|------|--------|
| Hardware watchdog (IWDG) | Firmware hang → motor runs | Not implemented |
| Software watchdog (Python mode) | Host crash → motor runs | Disabled |
| Software dead-time | Shoot-through during transition | Not implemented |
| Host connection monitor | Serial disconnect → motor runs | Not implemented |

---

## 11. 4-Motor Skid-Steer Architecture (Future)

### 11.1 System Topology

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

### 11.2 Motor Assignment (Skid-Steer)

| Motor | Position | Side |
|-------|----------|------|
| M1 | Front Left | Left |
| M2 | Front Right | Right |
| M3 | Rear Left | Left |
| M4 | Rear Right | Right |

### 11.3 Skid-Steer Control Logic

| Action | Left Motors | Right Motors |
|--------|-------------|--------------|
| Forward | f<duty> | f<duty> |
| Backward | b<duty> | b<duty> |
| Turn Left | b<duty> | f<duty> |
| Turn Right | f<duty> | b<duty> |
| Spin Left | b<duty> | f<duty> |
| Spin Right | f<duty> | b<duty> |
| Stop | s | s |

### 11.4 Scaling Points

The current single-motor driver code is designed to scale to 4 motors through:

1. **No global state coupling:** Each motor driver runs independently on its own STM32. There is no shared state between motor modules.

2. **UART-based communication:** Each motor driver accepts commands via UART. The hub STM32 can address each driver on a separate UART port.

3. **Protocol compatibility:** The f/b/s protocol is motor-agnostic. The hub simply routes commands to the appropriate motor.

4. **Telemetry multiplexing:** The hub STM32 can query each motor's telemetry and forward aggregated data to Python.

### 11.5 Hub STM32 Responsibilities (Separate Project)

- Receive commands from Python host (multi-motor protocol)
- Route commands to individual motor drivers via UART
- Collect telemetry from each motor driver
- Aggregate and forward telemetry to Python host
- Implement skid-steer logic (optional: hub vs Python)
- Handle motor-specific addressing (M1-M4)

### 11.6 Python Host for 4 Motors (Future)

The Python host will be updated to:
- Send multi-motor commands (e.g., `M1:f150`, `M2:f150`, `M3:b150`, `M4:b150`)
- Or send skid-steer commands (e.g., `TURN_LEFT:150`)
- Display telemetry for all 4 motors
- Support keyboard/gamepad input for skid-steer control

---

## 12. Design Decisions and Trade-Offs

### 12.1 60µs Control Tick

**Decision:** Motor control runs at 60µs (~16.6kHz) instead of a slower rate.

**Trade-off:** Higher CPU usage but smoother commutation and faster response to hall changes. At 8kHz PWM, this gives ~13 control ticks per PWM period, which is sufficient for smooth duty changes.

### 12.2 Deferred Command Application

**Decision:** Motor commands from UART are stored in `pendingReq` and applied during the motor control tick, not immediately when received.

**Trade-off:** Adds slight latency (up to 60µs) but ensures motor state changes happen atomically within the control tick, avoiding race conditions between UART processing and motor control.

### 12.3 Separate Python and Normal Mode Parsers

**Decision:** Python mode has its own command parser (`processPythonCommand`) separate from the normal CLI parser (`processCommand`).

**Trade-off:** Code duplication but clean separation. Python mode commands (w/s/x/d/a) have different semantics than normal mode commands (f/b/s/status/etc). The fallback from Python parser to normal parser (`processCommand(cmd, src)`) allows shared commands like "status", "mode", "identify" to work in both modes.

### 12.4 EEPROM-Based Configuration

**Decision:** Hall map, config, and operating mode are stored in EEPROM with magic numbers and checksums.

**Trade-off:** Limited EEPROM wear cycles (~100k for STM32 flash emulation) but persistent configuration across power cycles. Magic number validation prevents loading corrupted data.

### 12.5 Software-Only Dead-Time

**Decision:** Dead-time is not implemented in software. MOSFET switching relies on hardware characteristics.

**Trade-off:** Simpler code but potential shoot-through risk. Real-world impact depends on MOSFET driver speed and gate resistor values. See ISSUES.md ISSUE-02.

---

## 13. Known Architectural Risks

| Risk | Category | Impact | Mitigation |
|------|----------|--------|------------|
| No hardware watchdog | Safety | Firmware hang → motor runs | Add IWDG |
| No Python watchdog | Safety | Host crash → motor runs | Enable watchdog in Python mode |
| Single command/iteration | Performance | Queue buildup under load | Change `if` to `while` |
| Hardcoded pin mapping | Flexibility | Requires code change for different boards | Use board-specific config |
| No motor ID in protocol | Scalability | Hub can't address specific motors | Add motor ID prefix |
| PWM field semantics differ | Data integrity | Telemetry misinterpretation | Unify field naming |
