# Comprehensive Audit Report — bldc-project-stm

**Date:** 2026-06-25
**Scope:** All files except `h7-main/` (excluded per project rules) and `f411-motor/` (legacy reference only)

---

## CRITICAL — Safety / Correctness (Must Fix)

### C-01: Tools parse `PH` but firmware emits `APP_PH` ✅ FIXED
- **Files:** `tools/terminal.py` (line 779), `tools/ftdi_h7_client.py` (lines 32, 48, 78)
- **Fix applied:** Added `APP_PH` key to both tools, keeping `PH` as fallback. Updated docstrings and comments.

### C-02: F446 bridge sends undefined `estop` to F411 ✅ FIXED
- **File:** `f446-bridge-test/src/main.cpp` (line 75-86)
- **Fix applied:** Changed `sendToMotor(i, "estop")` to `sendToMotor(i, "safe")`. Updated comment, response message, and help text.

### C-03: F446 service unlock timeout not overflow-safe ✅ FIXED
- **File:** `f446-bridge-test/src/main.cpp` (lines 190, 424)
- **Fix applied:** Changed both `millis() > serviceUnlockMs` to `(millis() - serviceUnlockMs) >= SERVICE_TIMEOUT_MS`.

### C-04: `tools/ftdi_h7_emulator.py` allows duty 255, protocol max is 250 ✅ FIXED
- **File:** `tools/ftdi_h7_emulator.py` (lines 151, 286)
- **Fix applied:** Clamped to `min(250, rpm)` and updated help text to `0-250`.

### C-05: Smoke test expects wrong F446 stop response ✅ FIXED
- **File:** `tools/f446_serial_smoke_test.py` (line 127)
- **Fix applied:** Changed expectation to `"OK|safe stop"` (matching actual bridge output).

### C-06: Duty mode bare `f`/`b` direction reversal resets `target_duty` to `default_pwm` ✅ FIXED
- **File:** `f411-motor-cube/App/Src/app_main.c` (lines 546-551, 591-596)
- **Fix applied:** Removed `s_app.target_duty = s_app.default_pwm` from direction reversal branches. `target_duty` is preserved.

---

## HIGH — Code Quality / Robustness (Should Fix)

### H-01: `uart_protocol.c` missing `volatile` on ISR-shared variables ✅ FIXED
- **File:** `f411-motor-cube/App/Src/uart_protocol.c` (lines 72-76)
- **Fix applied:** Added `volatile` to `s_last_byte_ms` and `s_rx_drop_count` (ISR-written). Others are main-loop-only.

### H-02: F446 `isDangerousServiceCmd()` includes `defaults` ✅ FIXED
- **File:** `f446-bridge-test/src/main.cpp` (line 112)
- **Fix applied:** Removed `defaults` from dangerous list, added to direct passthrough.

### H-03: F446 direct passthrough asymmetry for kick/ramp ✅ FIXED
- **File:** `f446-bridge-test/src/main.cpp` (lines 139-153)
- **Fix applied:** Added `kick on`, `ramp off`, `defaults`, `map default`, `mapreset`, `reload` to direct passthrough.

### H-04: F446 `printHelp()` missing commands ✅ FIXED
- **File:** `f446-bridge-test/src/main.cpp` (lines 169-183)
- **Fix applied:** Added `bridge unlock_service`, `bridge lock_service`, `bridge status`, `all` to help text. Updated emergency stop description.

---

## MEDIUM — Documentation Inconsistencies (Should Fix)

### M-01: `docs/F446_BRIDGE.md` telemetry example uses `PH` not `APP_PH` ✅ FIXED
- **Fix applied:** Updated telemetry example to `APP_PH`.

### M-02: `docs/F446_BRIDGE.md` and `f446-bridge-test/README.md` gatetest duty 20 ✅ FIXED
- **Fix applied:** Changed to `gatetest 0 10`.

### M-03: F446 docs claim `estop` creates fault latch on F411 ✅ FIXED
- **Fix applied:** Updated to reflect actual behavior (coast stop, no fault latch).

### M-04: `docs/REVIEW_REPORT.md` P1-04 cites wrong file ✅ FIXED
- **Fix applied:** Corrected to note `terminal.py` and `ftdi_h7_client.py` as affected files. Also corrected P1-03, P1-05, P1-07, P1-08 findings. Updated Senaryo 5.

### M-05: F446 README passthrough list misleading ✅ FIXED
- **Fix applied:** Added `defaults` to passthrough list. Note: service commands (identify, scan, test, gatetest, save, etc.) correctly require unlock.

### M-06: `ftdi_h7_emulator.py --save-map` silently fails ✅ FIXED
- **Fix applied:** Added warning print that persistent storage is disabled.

---

## LOW — Cosmetic / Nice-to-Have

### L-01: `ftdi_h7_gui.py` default PWM slider 150 (bring-up starts at 10-20)
### L-02: `ftdi_h7_gui.py` "Target" label misleading in speed mode
### L-03: `ftdi_h7_gui.py` E-STOP button sends `safe` not `estop` (no fault latch)
### L-04: `bldc_commutation.c` has unused functions `Commutation_IsCompleteHallMap` / `Commutation_HasDuplicateSectors`
### L-05: `app_config.h` `SPEED_PI_MAX_PWM_SOFT_LIMIT` is dead config
### L-06: `app_main.c` `last_loop_ms` field is set but never read
### L-07: F446 `isDirectPassthrough()` missing `map default`, `mapreset`, `reload` ✅ FIXED (moved to H-03)

---

## Summary

| Category | Count | Fixed | Deferred |
|----------|-------|-------|----------|
| CRITICAL | 6 | 6 | 0 |
| HIGH | 4 | 4 | 0 |
| MEDIUM | 6 | 6 | 0 |
| LOW | 7 | 1 | 6 |

**Total fixed: 17** (all C + H + M + L-07)
**Deferred: 6** (L-01 through L-06: cosmetic GUI defaults, dead config, unused code)
