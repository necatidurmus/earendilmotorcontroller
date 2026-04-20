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

**Status:** ✅ CLOSED — L6388 driver 351ns dead-time sağlıyor, software dead-time gerekli değil

Backup kod karşılaştırması: mevcut ve backup aynı `applyDriveState()` koduna sahip, motorlar sorunsuz çalışmış. L6388 half-bridge driver donanımsal dead-time sağladığı için software dead-time eklemek gereksiz ve duty kaybına neden olur.

---

### ISSUE-03: Watchdog Disabled in Python Mode

**Status:** ✅ FIXED (v0.3.0)

`loop()` Python branch'te `checkCommandWatchdog(nowMs)` eklendi. `processPythonCommand()` içinde `f` ve `b` komutları `lastMotorCommandMs = millis()` ile lease yeniliyor. Python host ~600ms heartbeat ile watchdog timeout (800ms) öncesinde komut göndermeli.

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

**Status:** ✅ FIXED (v0.5.1)

IWatchdog kütüphanesi eklendi. `setup()`'ta 500ms timeout ile IWDG başlatılıyor. `loop()` sonunda `IWatchdog.reload()` çağrılıyor. IWDG reset tespit edilirse loglanıyor. Main loop 500ms'den fazla takılırsa MCU otomatik reset atar.

---

### ISSUE-09: RPM Calculation Comment is Incorrect

**Status:** ✅ FIXED

Comment düzeltildi: "15 pole pair" → "15 elektriksel tur/mekanik tur".

---

### ISSUE-10: `beginRunRequest` Same-Direction Duty Update is Unclear

**Status:** ✅ FIXED

`beginRunRequest()` erken dönüşüne yorum eklendi: duty güncellemesi `applyPendingRequests()` içinde yapıldığı için burada tekrar gerek yok. f/b/s protokolü `hasTargetDutyUpdate` set ediyor, duty doğru işleniyor.

---

### ISSUE-11: Ring Buffer / Command Queue Has No Timestamp

**Status:** ✅ FIXED (v0.3.0)

`CommandItem` struct'una `timestampMs` eklendi. `enqueueCommand()` timestamp yazıyor. `dequeueCommand()` `CMD_STALE_MS` (500ms) üzerindeki komutları discard ediyor.

---

### ISSUE-12: `lastMotorCommandMs` Update Inconsistency

**Status:** ✅ FIXED (v0.3.0)

Python modunda `w` ve `s` komutlarına `lastMotorCommandMs = millis()` eklendi. Normal modda `pwm <val>` komutuna da eklendi. `s` komutu `lastMotorCommandMs = 0` yapıyor (watchdog devre dışı bırakma), bu davranış tutarlı. Tüm motion komutları lease yeniliyor.

---

## False / Exaggerated / Unverified Claims

The following issues from the previous `problems.md` have been re-evaluated and classified as incorrect, exaggerated, or unverifiable.

### FALSE-01: Python Mode "s" Command Conflict with Stop

**Previous Claim:** In Python mode, "s" (backward) falls through to `processCommand()` which interprets it as "stop".

**Status:** IRRELEVANT — Phase 2 f/b/s protokolüne geçildi. Artık Python modunda `s` = stop, `b` = backward. Conflict yok.

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
| ISSUE-01 | ✅ Fixed | Bug | main.cpp | Queue processes only 1 command/iteration → if→while |
| ISSUE-02 | ✅ Closed | Safety | main.cpp:629-633 | L6388 351ns dead-time var, software gerekli değil |
| ISSUE-03 | ✅ Fixed | Safety Risk | main.cpp:2303 | Watchdog disabled in Python mode |
| ISSUE-04 | ✅ Fixed | Design | main.cpp:133 | Default PWM hardcoded to 60 → EEPROM configurable (150) |
| ISSUE-05 | ✅ Fixed | Design | main.cpp:1950 | Telemetry "PWM" field → PWM_SET/PWM_ACT + backward compat |
| ISSUE-06 | ✅ Fixed | Bug | main.cpp:1610 | saveall doesn't save operating mode → saveModeToStorage added |
| ISSUE-07 | ✅ Fixed | Bug | main.cpp:74 | Identify step transition too fast (1ms) → 50ms |
| ISSUE-08 | ✅ Fixed | Safety Risk | main.cpp:2203, 2345 | No hardware watchdog (IWDG) → IWatchdog 500ms |
| ISSUE-09 | ✅ Fixed | Doc Bug | main.cpp:2105 | RPM calculation comment corrected |
| ISSUE-10 | ✅ Fixed | Design | main.cpp:1179 | beginRunRequest same-direction duty clarifikasyonu |
| ISSUE-11 | ✅ Fixed | Design | main.cpp:293, 990 | Command queue has no timestamps |
| ISSUE-12 | ✅ Fixed | Design | main.cpp (multiple) | lastMotorCommandMs update inconsistent |

| FALSE-01 | — | Resolved | main.cpp | "s" command conflict — f/b/s protokolüne geçildi, geçersiz |
| FALSE-02 | — | Exaggerated | wasd_controller.py | Heartbeat thread "useless" (f/b/s ile tekrar değerlendirilmeli) |
| FALSE-03 | — | Exaggerated | wasd_controller.py | Phase list missing entry (naming inconsistency only) |
| UNVERIFIED-01 | — | Unverified | main.cpp:loop() | UART processing delay from motor tick |
| UNVERIFIED-02 | — | Unverified | main.cpp:processRxRingToLines | UART parsing affects commutation timing |
