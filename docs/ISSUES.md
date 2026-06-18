# ISSUES — BLDC Motor Driver Module

> **Project:** STM32 BLDC Motor Driver (Black Pill F411CE)
> **Files:** `src/main.cpp`, `tools/wasd_controller.py`
> **Last Updated:** 18 June 2026
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

| ISSUE-13 | ✅ Fixed | Safety | main.cpp:2693 | Brake timeout eksik — 3000ms timeout eklendi |
| ISSUE-14 | ✅ Fixed | Safety | main.cpp:2870, 2885 | Watchdog brake durumunu kontrol etmiyor → isMotorBusy |
| ISSUE-15 | ✅ Fixed | Design | main.cpp:1467 | `isMotionCommand` `x` komutunu tanımıyor → eklendi |
| ISSUE-16 | ✅ Fixed | Design | main.cpp:1469, 1944, 2156 | `rpm`/`pwm` motion command ama lease yenilemiyor → rpm'e eklendi |
| ISSUE-17 | ✅ Fixed | Design | main.cpp:2637 | Control modunda `pwm` PID devre dışı bırakmıyor → eklendi |
| ISSUE-18 | ✅ Fixed | Design | main.cpp:72 | CMD_STALE_MS > CMD_WATCHDOG_MS → 600ms yapıldı |

---

## New Issues — Code Review 2026-06-18

### ISSUE-13: Brake Timeout Mechanism Missing

**Status:** ✅ FIXED (v0.6.1)
**Priority:** Critical (Safety)

Brake durumunda `brakeHoldMs` timeout implemente edilmemiş. Motor sonsuza kadar brake'te kalabilir (low-side short). Bu motor ısınması ve MOSFET hasarına neden olabilir.

**Expected:** Brake belirli bir süre sonra otomatik olarak Stopped (coast) durumuna geçmeli.
**Actual:** Brake süresiz devam eder. Sadece `s` komutu veya watchdog ile durdurulabilir.

**Code Reference:** `src/main.cpp:2693` — `allBrake()` çağrılıyor ama timeout yok

**Fix Required:**
- `SavedConfig` struct'una `brakeEnabled` ve `brakeHoldMs` field'ları ekle
- `motorControlTick`'te brake timeout kontrolü ekle
- Brake timeout sonunda otomatik Stopped'a geç

---

### ISSUE-14: Watchdog Does Not Cover Brake State

**Status:** ✅ FIXED (v0.6.1)
**Priority:** Critical (Safety)

`checkCommandWatchdog()` ve `checkHostConnection()` sadece `isMotorDriveActive()` kontrol ediyor (Kick/Running). Brake durumunda watchdog çalışmıyor.

**Risk:** Brake durumunda host çökerse veya UART koparsa motor sonsuza kadar brake'te kalır.

**Code Reference:**
- `src/main.cpp:2870` — `if (!isMotorDriveActive()) return;`
- `src/main.cpp:2885` — Aynı sorun

**Fix Required:**
- `isMotorDriveActive()` yerine `isMotorBusy()` kullan (Brake dahil)
- Veya brake durumunu ayrı kontrol et

---

### ISSUE-15: `isMotionCommand` Does Not Recognize `x` (Brake)

**Status:** ✅ FIXED (v0.6.1)
**Priority:** Medium

`isMotionCommand()` fonksiyonu `f`, `b`, `s` komutlarını tanıyor ama `x` (brake) tanımıyor. Bu yüzden brake komutu queue'da overwrite edilmiyor, birikebilir.

**Code Reference:** `src/main.cpp:1467` — `if (c == 'f' || c == 'b' || c == 's') return true;`

**Fix Required:**
- `isMotionCommand`'a `x` komutunu ekle
- Veya brake komutunu ayrı kategoride ele al

---

### ISSUE-16: `rpm`/`pwm` Commands Recognized as Motion but Don't Refresh Lease

**Status:** ✅ FIXED (v0.6.1)
**Priority:** Medium (Tutarlılık)

`isMotionCommand()` `pwm` ve `rpm` komutlarını motion command olarak tanıyor (queue overwrite). Ama `processCommand()`'da bu komutlar `lastMotorCommandMs` güncellemiyor. Bu tutarsızlık watchdog'a neden olabilir.

**Code Reference:**
- `src/main.cpp:1469-1470` — `pwm` ve `rpm` motion command olarak tanınıyor
- `src/main.cpp:1944-1953` — `pwm <val>` handler'ında `lastMotorCommandMs` yok
- `src/main.cpp:2156-2181` — `rpm <val>` handler'ında `lastMotorCommandMs` yok

**Fix Required:**
- Seçenek A: `isMotionCommand`'dan `pwm` ve `rpm`'i kaldır
- Seçenek B: `processCommand`'da `pwm` ve `rpm` handler'larına `lastMotorCommandMs = millis()` ekle

---

### ISSUE-17: `processControlCommand` Does Not Disable PID on `pwm` Command

**Status:** ✅ FIXED (v0.6.1)
**Priority:** Low

Control modunda `pwm <val>` komutu geldiğinde `disablePidMode()` çağrılmıyor. Speed modundayken `pwm` komutu gelirse PI kontrolü devam eder, duty değişikliği etkisiz kalır.

**Code Reference:** `src/main.cpp:2637` — `parseLongAfterPrefix(cmd, "pwm ", &pv)` handler'ında `disablePidMode()` yok

**Fix Required:**
- `pwm` handler'ına `if (pidRt.enabled) disablePidMode();` ekle

---

### ISSUE-18: `CMD_STALE_MS` (2000ms) > `CMD_WATCHDOG_MS` (800ms)

**Status:** ✅ FIXED (v0.6.1)
**Priority:** Low (Tutarlılık)

Stale detection süresi (2000ms) watchdog süresinden (800ms) büyük. Watchdog önce motoru durdurduğu için stale komutlar asla işlenmeyebilir. Bu intentional olabilir ama belgelenmeli.

**Code Reference:**
- `src/main.cpp:72` — `CMD_STALE_MS = 2000`
- `src/main.cpp:70` — `CMD_WATCHDOG_MS = 800`

**Fix Required:**
- Seçenek A: `CMD_STALE_MS`'i `CMD_WATCHDOG_MS`'den küçük yap (örn. 500ms)
- Seçenek B: Bu davranışı belgele (intentional ise)

---

## Speed PI Control — Düşük Hız RPM Kontrolü

**Durum:** ✅ Uygulandı (v0.6.0)

### Amaç

Mevcut PID RPM kontrolcüsü, düşük hızda (10-30 RPM) statik sürtünme, ramp ve ağır telek inertia'sı nedeniyle yetersiz kalıyordu. Sabit PWM ile kapalı döngü RPM kontrolü yapılamıyordu.

### Yapılan Değişiklikler

1. **PID → PI dönüşümü:** D terimi kaldırıldı, sadece P+I kullanılıyor
2. **Base PWM/feed-forward:** Düşük RPM'de statik sürtünmeyi aşmak için taban PWM eklendi
3. **Start boost:** Motor kalkışında PI integral ile çakışmayan ayrı boost fazı
4. **Target RPM ramp:** Ani RPM değişimlerini yumuşatmak için ramp mekanizması
5. **Conditional anti-windup:** Çıkış doygunluğunda integral birikimini önleme
6. **Hall kenar sayacı:** Start boost geçiş koşulları için hassas hall geçiş takibi

### Yeni CLI Komutları

| Komut | Açıklama |
|-------|----------|
| `mode speed` | Speed PI kontrol modunu aktifleştir |
| `mode duty` | Manuel PWM moduna dön |
| `rpm <value>` | Hedef RPM ayarla (signed: +ileri, -geri) |
| `pi <kp> <ki>` | PI kazançlarını ayarla |
| `base <lo> <mid> <hi>` | Base PWM değerlerini ayarla |
| `boost <lo> <mid> <hi> <ms>` | Start boost parametrelerini ayarla |
| `ramp <up> <down>` | RPM ramp hızlarını ayarla (RPM/saniye) |
| `spstat` | Speed PI detaylı durum raporu |

### Varsayılan Değerler

| Parametre | Değer | Açıklama |
|-----------|-------|----------|
| Kp | 0.80 | Oransal kazanç |
| Ki | 0.10 | Integral kazanç |
| Base PWM Low | 55 | Hedef RPM ≤ 30 |
| Base PWM Mid | 45 | Hedef RPM ≤ 150 |
| Base PWM High | 35 | Hedef RPM > 150 |
| Boost Low PWM | 65 | Kalkış boost (RPM ≤ 30) |
| Boost Mid PWM | 65 | Kalkış boost (RPM ≤ 150) |
| Boost High PWM | 65 | Kalkış boost (RPM > 150) |
| Boost Time | 150ms | Boost süresi |
| Boost Edge Threshold | 3 | Geçiş için gereken hall kenar sayısı |
| Ramp Up | 100 RPM/s | RPM artış hızı |
| Ramp Down | 200 RPM/s | RPM azalış hızı |
| PI Max PWM | 180 | PI çıkış üst sınırı |

### Durum Makinesi

```
mode speed → rpm <value> →
  Idle → (motor başlat) → StartBoost → (hall kenarları veya timeout) → SpeedPI
  SpeedPI → (rpm 0 veya stop) → Idle
```

### Test Prosedürü

**Adım 1 — Build:**
- `pio run` ile derle
- Compile error'ları düzelt

**Adım 2 — Duty mode sanity test:**
- Eski PWM modunu test et
- forward/backward/stop komutlarının çalıştığını doğrula
- Hall telemetrisinin hâlâ çalıştığını doğrula

**Adım 3 — RPM ölçüm testi:**
- Motoru duty mode'da farklı PWM değerlerinde çalıştır
- Ölçülen RPM'i gözlemle
- RPM'in duty ile mantıklı şekilde değiştiğini doğrula

**Adım 4 — Düşük hız speed mode testi (teker boşta):**
```
mode speed
rpm 10
rpm 15
rpm 23
```
- START_BOOST → SPEED_PI_CONTROL geçişini kontrol et
- spstat ile durumu izle

**Adım 5 — Düşük hız speed mode testi (teker yerde):**
- rpm 23 ile başla
- Sonra rpm 15 test et
- BASE_PWM_LOW ve START_BOOST_LOW_PWM ayarla
- Önce Ki = 0 ile Kp ayarla
- Sonra küçük Ki ekle

**Adım 6 — Orta/yüksek hız testleri:**
- 50, 100, 150 RPM test et
- Stabil davranıştan sonra 300-400 RPM test et (önce boşta)
- 400 RPM'i araç üzerinde test etmeden önce güvenlik doğrulaması yap

### Dikkat Edilecekler

- Current sense bu implementasyonda dahil edilmedi (donanım sorunları)
- Mevcut duty mode tamamen korundu
- `pid on/off` komutları geriye uyumluluk için korundu
- `kd` komutu artık bilgi mesajı döndürüyor (PI only)
- Telemetri formatı `PID:` → `SP:` olarak güncellendi
