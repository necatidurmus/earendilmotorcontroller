# ISSUES — BLDC Motor Driver Module

> **Project:** STM32 BLDC Motor Driver (Black Pill F411CE)
> **Files:** `src/main.cpp`, `tools/wasd_controller.py`
> **Last Updated:** 20 April 2026
> **Priority Levels:** Critical / High / Medium / Low

---

## Table of Contents

1. [Active Issues](#active-issues)
2. [False / Exaggerated / Unverified Claims](#false--exaggerated--unverified-claims)
3. [Summary Table](#summary-table)

---

## Active Issues

### ISSUE-01: Command Queue Processes Only 1 Command Per Loop

| Field | Detail |
|-------|--------|
| **Priority** | Critical |
| **Type** | Bug |
| **File** | `src/main.cpp:1689-1694`, `src/main.cpp:1853-1858` |
| **Function** | `processQueuedCommands()`, `processPythonQueuedCommands()` |

**Current Behavior:** Both functions use `if (dequeueCommand(&item))` which processes only one command per loop iteration. If multiple commands are queued, they accumulate and are processed one per loop cycle.

**Expected Behavior:** All queued commands should be drained within a single loop iteration.

**Technical Impact:** Under rapid command input or when motor control tick consumes significant time, the command queue fills up. Commands may be processed with increasing latency. In worst case, queue overflow occurs and commands are dropped (`queueOverflowFlag`).

**Recommended Fix:** Change `if` to `while` in both functions to drain the queue completely per iteration.

**Evidence Status:** Verified — code inspection confirms `if` instead of `while` at both locations.

---

### ISSUE-02: No Software Dead-Time in `applyDriveState`

| Field | Detail |
|-------|--------|
| **Priority** | High (test-dependent) |
| **Type** | Safety Risk |
| **File** | `src/main.cpp:614-632` |
| **Function** | `applyDriveState()` |

**Current Behavior:** When transitioning between drive states, old MOSFET pins are turned off and new pins are turned on with no delay between operations. The sequence is:
```cpp
if (oldH != newH) analogWrite(oldH, 0);
if (oldL != newL) digitalWrite(oldL, LOW);
if (oldL != newL) digitalWrite(newL, HIGH);
analogWrite(newH, dutyCycle);
```

**Expected Behavior:** There should be a dead-time (typically 1-5µs) between turning off one MOSFET pair and turning on the next to prevent shoot-through current.

**Technical Impact:** Without dead-time, there is a theoretical risk of both high-side and low-side MOSFETs conducting simultaneously during transition, causing shoot-through current, excessive heating, and potential MOSFET damage. Real-world impact depends on MOSFET driver characteristics and switching speed.

**Recommended Fix:** Add `delayMicroseconds(1-5)` between off and on transitions, or use hardware dead-time MOSFET drivers.

**Evidence Status:** Verified — code inspection confirms immediate switching with no delay. Real-world impact requires hardware testing.

---

### ISSUE-03: Watchdog Disabled in Python Mode

| Field | Detail |
|-------|--------|
| **Priority** | High (safety) |
| **Type** | Safety Risk |
| **File** | `src/main.cpp:2188-2198` |
| **Function** | `loop()` — Python mode branch |

**Current Behavior:** The `checkCommandWatchdog()` function is called in Normal and Settings modes but NOT in Python mode. The code comment explicitly states: "Python modunda watchdog yok — motor X ile durdurana kadar çalışır".

**Expected Behavior:** A watchdog mechanism should be active in all modes to ensure the motor stops if the host becomes unresponsive.

**Technical Impact:** If the Python host crashes, the serial connection drops, or the host freezes, the motor will continue running indefinitely at its last commanded duty. Only a power cycle or manual intervention can stop it. This is a safety hazard for unattended operation.

**Recommended Fix:** Enable `checkCommandWatchdog()` in Python mode as well. The lease-based heartbeat from Python (every ~600ms) combined with an 800ms watchdog timeout provides a safe failsafe.

**Evidence Status:** Verified — `loop()` function confirms watchdog is absent in Python mode branch. The `CMD_WATCHDOG_MS = 800` constant and `checkCommandWatchdog()` function exist and work correctly in Normal/Settings modes.

---

### ISSUE-04: Default PWM Value is 60, Not Configurable

| Field | Detail |
|-------|--------|
| **Priority** | Medium |
| **Type** | Design Inconsistency |
| **File** | `src/main.cpp:271`, `src/main.cpp:1648` |
| **Function** | Global declaration, `processCommand()` — mode python handler |

**Current Behavior:** `pythonPwmValue` is hardcoded to 60 at startup and when entering Python mode. The value is not stored in EEPROM or configurable.

**Expected Behavior:** Default PWM should be configurable (target: 150) and ideally persistent across reboots via EEPROM.

**Technical Impact:** Users must manually adjust PWM every time they enter Python mode. The low default (60) may be insufficient for some motors, while 150 may be too high for others. A configurable default improves usability.

**Recommended Fix:** Add `defaultPwm` field to `SavedConfig`, load it on startup, and use it as the initial `pythonPwmValue`.

**Evidence Status:** Verified — code confirms `pythonPwmValue = 60` at declaration and in mode switch handler.

---

### ISSUE-05: Telemetry Field "PWM" Has Different Semantics Per Mode

| Field | Detail |
|-------|--------|
| **Priority** | Medium |
| **Type** | Design Inconsistency |
| **File** | `src/main.cpp:2053-2054`, `src/main.cpp:1878-1879` |
| **Function** | `sendTelemetry()`, `sendPythonTelemetry()` |

**Current Behavior:**
- Normal mode `sendTelemetry()`: "PWM" field sends `motorRt.targetDuty` (firmware's target duty)
- Python mode `sendPythonTelemetry()`: "PWM" field sends `pythonPwmValue` (Python host's set value)

Both use the same field name "PWM" but with different meanings.

**Expected Behavior:** The telemetry protocol should use consistent field names and meanings across modes, or use distinct field names when values differ.

**Technical Impact:** If a telemetry consumer parses "PWM" expecting consistent semantics, it will get confused. The Python host controller happens to work because it only runs in Python mode, but future multi-motor hosts may misinterpret the data.

**Recommended Fix:** Use "PWM_SET" for the host-commanded value and "PWM_ACT" for the firmware's actual target duty. Or unify the semantics.

**Evidence Status:** Verified — code inspection confirms different values assigned to "PWM" field in different mode functions.

---

### ISSUE-06: `saveall` Command Does Not Save Operating Mode

| Field | Detail |
|-------|--------|
| **Priority** | Medium |
| **Type** | Bug |
| **File** | `src/main.cpp:1538-1552` (approximate, in processCommand) |
| **Function** | `processCommand()` — saveall handler |

**Current Behavior:** The `saveall` command saves hall map and config to EEPROM but does not call `saveModeToStorage()`. The current operating mode is not persisted.

**Expected Behavior:** `saveall` should save all persistent state including the operating mode.

**Technical Impact:** After `saveall` and reboot, the device reverts to Normal mode instead of staying in the previously active mode (Python or Settings).

**Recommended Fix:** Add `saveModeToStorage()` call to the `saveall` handler.

**Evidence Status:** Verified — code inspection confirms `saveall` calls `saveHallMapToStorage()` and `saveConfigToStorage()` but not `saveModeToStorage()`.

---

### ISSUE-07: Identify Step Transition Too Fast

| Field | Detail |
|-------|--------|
| **Priority** | Medium |
| **Type** | Bug |
| **File** | `src/main.cpp:1326` (approximate, in updateServiceIdentify) |
| **Function** | `updateServiceIdentify()` |

**Current Behavior:** `serviceRt.nextActionMs = now + IDENTIFY_TOGGLE_MS` where `IDENTIFY_TOGGLE_MS = 1`. The step transitions happen every 1ms.

**Expected Behavior:** Motor needs physical settling time between steps. 1ms is insufficient for the motor to physically respond.

**Technical Impact:** Identify may produce invalid or inconsistent hall maps because the motor hasn't physically moved to the next commutation step before the hall is read.

**Recommended Fix:** Increase `IDENTIFY_TOGGLE_MS` to at least 50-100ms, or add a separate settling delay constant.

**Evidence Status:** Verified — code and constant definition confirm 1ms step transition.

---

### ISSUE-08: No Hardware Watchdog (IWDG)

| Field | Detail |
|-------|--------|
| **Priority** | High (safety) |
| **Type** | Safety Risk |
| **File** | Entire project |
| **Function** | N/A — feature missing |

**Current Behavior:** No IWDG (Independent Watchdog) initialization or feeding anywhere in the code. Only software watchdog (`CMD_WATCHDOG_MS = 800`) exists, which is disabled in Python mode.

**Expected Behavior:** A hardware watchdog should be present as a second safety layer. If the firmware hangs (e.g., in a tight loop, ISR deadlock, or memory corruption), the IWDG will reset the MCU and turn off all motor outputs.

**Technical Impact:** If the firmware enters an unrecoverable state (stack overflow, infinite loop, ISR deadlock), the motor may continue running at whatever duty was last applied. The software watchdog cannot help if the main loop is stuck.

**Recommended Fix:** Initialize IWDG with a timeout (e.g., 500ms) in `setup()`, feed it in `loop()`. This provides a hardware-level failsafe independent of the software watchdog.

**Evidence Status:** Verified — no `#include <IWDG.h>`, no `IWDG` references found in the entire codebase.

---

### ISSUE-09: RPM Calculation Comment is Incorrect

| Field | Detail |
|-------|--------|
| **Priority** | Low |
| **Type** | Documentation Bug |
| **File** | `src/main.cpp:2022, 2031` |
| **Function** | `calculateRPM()` |

**Current Behavior:** Comment states "6 hall geçişi/elektriksel tur × 15 pole pair = 90 geçiş/mekanik tur". This is grammatically misleading.

**Expected Behavior:** The correct phrasing is: "6 transitions/electrical revolution × 15 electrical revolutions/mechanical revolution = 90 transitions/mechanical revolution".

**Technical Impact:** The code is correct (multiplies by 90), but the comment may confuse future developers about the relationship between pole pairs and electrical revolutions.

**Evidence Status:** Verified — comment exists at the specified location.

---

### ISSUE-10: `beginRunRequest` Same-Direction Duty Update is Unclear

| Field | Detail |
|-------|--------|
| **Priority** | Low |
| **Type** | Design Issue |
| **File** | `src/main.cpp:1003-1010` (approximate) |
| **Function** | `beginRunRequest()` |

**Current Behavior:** When motor is already driving in the same direction, `beginRunRequest()` returns early without updating duty. Duty updates rely on `pendingReq.hasTargetDutyUpdate` which is handled separately in `applyPendingRequests()`.

**Expected Behavior:** The relationship between `beginRunRequest()` and duty updates should be clearer, especially for the f/b/s protocol where `f150` means "run forward at PWM 150".

**Technical Impact:** With the upcoming f/b/s protocol, every `f150` command should update both direction AND duty. The current separation may cause duty updates to be missed if `pendingReq.hasTargetDutyUpdate` is not set by the command parser.

**Recommended Fix:** Ensure the f/b/s command parser always sets `hasTargetDutyUpdate = true` when a duty value is provided, regardless of current direction.

**Evidence Status:** Verified — code shows early return on same direction without explicit duty update.

---

### ISSUE-11: Ring Buffer / Command Queue Has No Timestamp

| Field | Detail |
|-------|--------|
| **Priority** | Low |
| **Type** | Design Concern |
| **File** | `src/main.cpp:929-938` (enqueueCommand), `src/main.cpp:940-947` (dequeueCommand) |
| **Function** | `enqueueCommand()`, `dequeueCommand()` |

**Current Behavior:** Commands are queued without timestamps. The `RxRing` and `CommandQueue` structures store command text and source but not arrival time.

**Expected Behavior:** Commands could be timestamped on enqueue to allow stale command detection and discard.

**Technical Impact:** Under high load or slow processing, old commands may be executed long after they were sent. For motion commands, this could result in unexpected motor behavior (e.g., a stop command from 2 seconds ago being applied after a new forward command).

**Recommended Fix:** Add timestamp field to `CommandItem`. Discard commands older than a threshold (e.g., 500ms) in `dequeueCommand()`.

**Evidence Status:** Verified — `CommandItem` struct has no timestamp field.

---

### ISSUE-12: `lastMotorCommandMs` Update Inconsistency

| Field | Detail |
|-------|--------|
| **Priority** | Low |
| **Type** | Design Inconsistency |
| **File** | `src/main.cpp` (multiple locations in processCommand, processPythonCommand) |
| **Function** | `processCommand()`, `processPythonCommand()` |

**Current Behavior:** `lastMotorCommandMs` is updated in some command handlers (f/b in processCommand, w/s/d/a in processPythonCommand) but not consistently. Some non-motion commands that should reset the watchdog timer don't update it.

**Expected Behavior:** Any valid command should refresh the lease/timestamp to indicate "host is alive". Alternatively, only motion commands should refresh it — but the policy should be explicit.

**Technical Impact:** With the watchdog enabled, a host sending only non-motion commands (e.g., repeated "status" queries) while the motor is running would cause a watchdog stop.

**Recommended Fix:** Define a clear policy: either all commands refresh the lease, or only motion commands do. Document the chosen policy.

**Evidence Status:** Verified — code inspection shows `lastMotorCommandMs` is set in some handlers but not others.

---

## False / Exaggerated / Unverified Claims

The following issues from the previous `problems.md` have been re-evaluated and classified as incorrect, exaggerated, or unverifiable.

### FALSE-01: Python Mode "s" Command Conflict with Stop

**Previous Claim:** In Python mode, "s" (backward) falls through to `processCommand()` which interprets it as "stop".

**Reality:** `processPythonCommand()` has an explicit `strcmp(cmd, "s") == 0` check at line 1779 that catches "s" as backward and returns. The fallthrough to `processCommand()` only occurs for commands that match NO Python-specific handler. "s" is always caught by the Python handler.

**Status:** FALSE — wasd_controller.py sends "s" → processPythonCommand catches it → handles as backward → returns. No conflict.

**Note:** This issue becomes irrelevant with the upcoming f/b/s protocol transition (WASD commands will be removed).

---

### FALSE-02: Heartbeat Thread is Completely Useless

**Previous Claim:** The heartbeat thread (`_sender_loop`) never fires because `key_held` is cleared immediately by the main loop.

**Reality:** This is true for the CURRENT implementation with `nodelay(True)` + `timeout(80)`. However, the heartbeat thread IS correctly structured — it would work if the hold-to-run mechanism were fixed. It's not "dead code" in principle, just never reached due to a separate bug.

**Status:** PARTIALLY FALSE — the thread is correctly designed but unreachable due to ISSUE-01's hold-to-run bug. With the protocol transition to f/b/s, this thread will be redesigned anyway.

---

### FALSE-03: Phase Names List is Missing Entry

**Previous Claim:** `phases = ["STOPPED", "KICK", "RUNNING", "NEUTRAL", "FAULT"]` is missing "NEUTRAL_WAIT".

**Reality:** The firmware sends `MotorPhase` enum values 0-4. The list has 5 entries (indices 0-4). `NeutralWait = 3` maps to `phases[3]` which is "NEUTRAL". This is a naming inconsistency (should be "NEUTRAL_WAIT") but NOT a missing entry — no IndexError would occur.

**Status:** EXAGGERATED — it's a naming inconsistency, not a missing entry. The display shows "NEUTRAL" instead of "NEUTRAL_WAIT".

---

### UNVERIFIED-01: Motor Control Tick Can Delay UART Processing

**Previous Claim:** If `motorControlTick()` takes too long, UART processing and command handling are delayed.

**Reality:** `motorControlTick()` runs at 60µs intervals with a maximum of 4 catch-up ticks (240µs total). This is unlikely to cause significant UART processing delay in practice. The claim is theoretically valid but unverified as a real problem.

**Status:** UNVERIFIED — theoretically possible but not confirmed as a practical issue.

---

### UNVERIFIED-02: UART Parsing Affects Commutation Timing

**Previous Claim:** UART line parsing in `processRxRingToLines()` could interfere with motor commutation.

**Reality:** UART processing happens in `loop()` after `runMotorControlScheduler()`. The motor control scheduler runs first, then UART. Since `motorControlTick` is time-critical and runs before UART processing, this concern is unlikely to be valid. However, if the loop iteration takes too long, the next control tick could be delayed.

**Status:** UNVERIFIED — motor control runs before UART processing in the loop order, making this unlikely.

---

## Summary Table

| ID | Priority | Type | File | Summary |
|----|----------|------|------|---------|
| ISSUE-01 | Critical | Bug | main.cpp:1689, 1853 | Queue processes only 1 command/iteration |
| ISSUE-02 | High | Safety Risk | main.cpp:614-632 | No software dead-time in drive transitions |
| ISSUE-03 | High | Safety Risk | main.cpp:2188-2198 | Watchdog disabled in Python mode |
| ISSUE-04 | Medium | Design | main.cpp:271, 1648 | Default PWM hardcoded to 60, not configurable |
| ISSUE-05 | Medium | Design | main.cpp:2053, 1878 | Telemetry "PWM" field has different semantics per mode |
| ISSUE-06 | Medium | Bug | main.cpp:1538-1552 | saveall doesn't save operating mode |
| ISSUE-07 | Medium | Bug | main.cpp:1326 | Identify step transition too fast (1ms) |
| ISSUE-08 | High | Safety Risk | Entire project | No hardware watchdog (IWDG) |
| ISSUE-09 | Low | Doc Bug | main.cpp:2022 | RPM calculation comment incorrect |
| ISSUE-10 | Low | Design | main.cpp:1003-1010 | beginRunRequest same-direction duty update unclear |
| ISSUE-11 | Low | Design | main.cpp:929-938 | Command queue has no timestamps |
| ISSUE-12 | Low | Design | main.cpp (multiple) | lastMotorCommandMs update inconsistent |

| FALSE-01 | — | False Claim | main.cpp:1779 | "s" command conflict (Python handler catches it) |
| FALSE-02 | — | Exaggerated | wasd_controller.py:127 | Heartbeat thread "useless" (correctly designed, unreachable) |
| FALSE-03 | — | Exaggerated | wasd_controller.py:203 | Phase list missing entry (naming inconsistency only) |
| UNVERIFIED-01 | — | Unverified | main.cpp:loop() | UART processing delay from motor tick |
| UNVERIFIED-02 | — | Unverified | main.cpp:processRxRingToLines | UART parsing affects commutation |
