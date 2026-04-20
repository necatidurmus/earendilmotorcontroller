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

**Status:** ✅ FIXED (v0.3.0)

`processQueuedCommands()` ve `processPythonQueuedCommands()` içinde `if` → `while` değiştirildi. `CMD_QUEUE_LEN` (8) budget ile loop starvation önlendi.

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

**Status:** ✅ FIXED (v0.3.0)

`loop()` Python branch'te `checkCommandWatchdog(nowMs)` eklendi. `processPythonCommand()` içinde `w` ve `s` komutları `lastMotorCommandMs = millis()` ile lease yeniliyor. Python host ~600ms heartbeat ile watchdog timeout (800ms) öncesinde komut göndermeli.

---

### ISSUE-04: Default PWM Value is 60, Not Configurable

**Status:** ✅ FIXED (v0.3.0)

`SavedConfig` struct'una `defaultPwm` alanı eklendi. EEPROM'dan yükleniyor. Varsayılan değer 150. `defpwm <0-255>` CLI komutu ile ayarlanabilir. `savecfg`/`saveall` ile persist ediliyor.

---

### ISSUE-05: Telemetry Field "PWM" Has Different Semantics Per Mode

**Status:** ✅ FIXED (v0.3.0)

Normal modda `PWM_ACT:` (firmware target duty), Python modda `PWM_SET:` (host set value) + `PWM_ACT:` eklendi. Backward compat için `PWM:` alanı her iki modda da korunuyor. Python host `PWM_SET` öncelikli parse yapıyor.

---

### ISSUE-06: `saveall` Command Does Not Save Operating Mode

**Status:** ✅ FIXED (v0.3.0)

`saveall` handler'ına `saveModeToStorage()` çağrısı eklendi. Artık hall map + config + mode birlikte persist ediliyor. Çıktıya `mode=OK/FAIL` bilgisi eklendi.

---

### ISSUE-07: Identify Step Transition Too Fast

**Status:** ✅ FIXED (v0.3.0)

`IDENTIFY_TOGGLE_MS` 1ms → 50ms. Yeni `IDENTIFY_STEP_INTERVAL_MS` (50ms) sabiti eklendi. Step geçişleri artık motorun fiziksel yanıt verebileceği sürede gerçekleşiyor.

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

**Status:** ✅ FIXED (v0.3.0)

Python modunda `w` ve `s` komutlarına `lastMotorCommandMs = millis()` eklendi. Normal modda `pwm <val>` komutuna da eklendi. `s` komutu `lastMotorCommandMs = 0` yapıyor (watchdog devre dışı bırakma), bu davranış tutarlı. Tüm motion komutları lease yeniliyor.

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
| ISSUE-01 | ✅ Fixed | Bug | main.cpp:1689, 1853 | Queue processes only 1 command/iteration → if→while |
| ISSUE-02 | High | Safety Risk | main.cpp:614-632 | No software dead-time in drive transitions |
| ISSUE-03 | High | Safety Risk | main.cpp:2188-2198 | Watchdog disabled in Python mode |
| ISSUE-04 | ✅ Fixed | Design | main.cpp:271, 1648 | Default PWM hardcoded to 60 → EEPROM configurable (150) |
| ISSUE-05 | ✅ Fixed | Design | main.cpp:2053, 1878 | Telemetry "PWM" field → PWM_SET/PWM_ACT + backward compat |
| ISSUE-06 | ✅ Fixed | Bug | main.cpp:1538-1552 | saveall doesn't save operating mode → saveModeToStorage added |
| ISSUE-07 | ✅ Fixed | Bug | main.cpp:1326 | Identify step transition too fast (1ms) → 50ms |
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
