# Project Analysis: Potential Risks, Bottlenecks, and Flaws

## 1. CRITICAL — `TerminalInterface::update()` locks up after first command

**File:** `terminal_interface.cpp` lines 9-12, 47-49

The `_lineReady` flag is set to `true` inside `update()` but is never reset back to `false` after `readLine()` is called. `resetBuffer()` exists but is never called from `main.cpp`. Result: after the first command is processed, `update()` returns `false` forever and the system locks up.

```
main.cpp lines 48-50:
    if (!terminal.update()) {
        return;  // always returns here while _lineReady stays true
    }
```

`_lineReady = false` and `_linePos = 0` must be performed after `readLine()` is consumed.

---

## 2. CRITICAL — No response reading from F411 cards

**File:** `motor_dispatcher.cpp`, `status_manager.cpp`

The system sends commands to F411 cards but never reads responses. `MotorDispatcher` only performs writing (`write`/`flush`), there is no `read()` call on any UART. `ProtocolHandler::encodeGetStatus()` is defined but never used anywhere. The `status` command displays motor state based only on what H7 last sent — not actual motor state.

---

## 3. RISK — `uart->flush()` blocks the main loop

**File:** `motor_dispatcher.cpp` line 28

The `flush()` call blocks the CPU until the TX buffer is completely empty. At 115200 baud, a 6-byte packet takes ~0.5ms. With 4 motors sent sequentially, ~2ms of blocking occurs per loop iteration. Acceptable for now, but becomes a problem if UART speed is reduced or packet size increases.

---

## 4. RISK — Extra parameters are silently swallowed

**File:** `command_parser.cpp` lines 58-65

A command like `forward 200 garbage` parses successfully because only the second token (`200`) is consumed and remaining characters are not checked. This hides user errors.

---

## 5. RISK — Arduino `String` usage causes heap fragmentation

**File:** `status_manager.cpp` lines 56-73, `main.cpp` lines 117-145

`printStatus()` and confirmation message generation create `String` objects via concatenation. Each `+` operator causes a new heap allocation. Over long-term use, this risks heap fragmentation and memory leaks. Embedded systems should prefer `snprintf` or `char[]` buffers.

---

## 6. RISK — Motor status updated even after failed send

**File:** `main.cpp` lines 108-110

```cpp
if (!ok) {
    allSuccess = false;
    // ... error message ...
}
// This line runs regardless:
status.updateMotor(motors[i].motorId, motors[i].direction, motors[i].pwm);
```

Even when sending fails, motor status is updated as if it succeeded. The `status` output becomes misleading.

---

## 7. RISK — `PWM_DEFAULT` and `status.terminalConnected` are unused

**File:** `config.h` line 28, `types.h` line 83

`PWM_DEFAULT` (128) is defined but never used anywhere. `SystemState::terminalConnected` is always `false` and never updated. These create dead code and maintenance burden.

---

## 8. LOW RISK — Serial3 build flag may be missing

**File:** `platformio.ini` lines 15-18

Build flags for `Serial2`, `Serial4`, `Serial5` are added but `Serial3` (RL motor) is not included. USART3 may be enabled by default on NUCLEO-H723ZG, but this can vary depending on the board package version. Build currently succeeds but may fail with a different `ststm32` platform version.

---

## 9. LOW RISK — Unnecessary include in `status_manager.cpp`

**File:** `status_manager.cpp` line 4

`#include "terminal_interface.h"` is included solely for the `extern TerminalInterface terminal;` declaration. Since the header does not declare the global `terminal`, this extern declaration is necessary, but the full class definition is not needed here — a forward declaration would suffice. This is not a functional issue, just an unnecessary dependency.

---

## SUMMARY TABLE

| # | Issue | Priority | Impact |
|---|-------|----------|--------|
| 1 | `_lineReady` never reset, system locks up | **CRITICAL** | System stops after first command |
| 2 | No response reading from F411 | **CRITICAL** | Motor status is not real |
| 3 | `flush()` blocks | RISK | Loop delay |
| 4 | Extra parameters swallowed | RISK | Hidden user error |
| 5 | `String` heap fragmentation | RISK | Long-term memory issues |
| 6 | Status updated on failed send | RISK | Incorrect status output |
| 7 | Dead code (`PWM_DEFAULT`, `terminalConnected`) | RISK | Maintenance burden |
| 8 | Serial3 flag potentially missing | LOW | Depends on platform version |
| 9 | Unnecessary include | LOW | Architecture cleanliness |

**Issue #1 must be fixed before the system can operate.** It will never process a second command.
