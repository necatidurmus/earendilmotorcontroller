# F446 Bridge Yapılarının H7'ye Entegrasyon Planı

> **Hedef:** `f446-bridge-test/` içindeki güvenlik ve kontrol yapılarını
> `h7-main/` rover controller mimarisine uygun şekilde entegre etmek.
>
> **Prensip:** F446 kodu birebir kopyalanmayacak; **konseptler** H7'nin
> mevcut katmanlı yapısına (`terminal_parser → command_handler →
> motor_dispatcher → motor_tx_dma`) adapte edilecek.

---

## 1. Mevcut Durum Analizi

### H7'de Zaten Var Olan Karşılıklar

| F446 Yapısı | H7 Karşılığı | Durum |
|-------------|-------------|-------|
| `isValidFDuty()` / `isValidBDuty()` | `terminal_parser.c`'de `allDigits()` + `FillMotion()` | **H7 daha gelişmiş** — tuning komut validasyonu da var |
| `isDirectPassthrough()` | `terminal_parser.c`'deki komut eşleştirme mantığı | **H7 daha kapsamlı** — tüm komut tipleri ayrıştırılıyor |
| `isDangerousServiceCmd()` | Yok (DISARM gate var ama komut-seviye filtreleme yok) | **Eksik** — eklenecek |
| `strEqNoCase()` | `terminal_parser.c`'de `tolower()` ile normalize ediliyor | **H7 farklı yaklaşım** — satırı baştan küçük harfe çeviriyor |
| `trimInPlace()` | `terminal_parser.c`'de `isspace()` ile trim | **Aynı** |
| Servis kilidi (30sn timeout) | Yok (DISARM daha kaba bir güvenlik katmanı) | **Eksik** — eklenecek |
| İki aşamalı stop | `MotionController_Stop()` direkt stop gönderir | **Eksik** — eklenecek |
| `estop` | Yok | **Eksik** — eklenecek |
| `bridge on/off/status` | Yok (tüm telemetri loglanır) | **Eksik** — eklenecek |

### F446'da Olmayıp H7'de Olan Yapılar

- 4 motor UART yönetimi (DMA RX/TX)
- Rover mode (DISARM/MANUAL/AUTONOMOUS) state machine
- Tuning komut validasyonu (base/boost/pi/ramp/kickduty/kickms/telper)
- ACK timeout/retry mekanizması
- Link-loss tespiti (3sn timeout)
- Tank-drive steering (f/b/r/l + fd/bd/rd/ld)
- Defense-in-depth DISARM gate (`motor_dispatcher.c:31`)
- DMA buffer'ları non-cacheable RAM_D2'de

---

## 2. Plan: 5 Aşama

Her aşama bağımsız commit olacak şekilde tasarlanmıştır. Sıra önemlidir
çünkü sonraki aşamalar öncekilere bağımlıdır.

---

### Aşama 1: Servis Kilidi Modülü (`service_lock.c/h`)

**Amaç:** F446'daki `bridge unlock_service` mekanizmasını H7'ye taşımak.
Bu, DISARM'dan bağımsız ikinci bir güvenlik katmanıdır — DISARM
rover-level kilitleme iken, service lock F411'e giden tehlikeli
komutlar (base/boost/pi/gatetest/save/identify/scan/test) için
ek koruma sağlar.

**Yeni dosyalar:**
```
Core/Inc/service_lock.h
Core/Src/service_lock.c
```

**`service_lock.h` — Public API:**
```c
#ifndef SERVICE_LOCK_H
#define SERVICE_LOCK_H

#include <stdbool.h>
#include <stdint.h>

#define SERVICE_TIMEOUT_MS  30000u
#define SERVICE_TOKEN       "CURRENT_LIMITED_BENCH_SUPPLY"

/* Status query result */
typedef struct {
    bool     unlocked;
    uint32_t remaining_ms;   /* 0 if locked */
    uint32_t blocked_count;  /* ömür boyu sayaç */
} ServiceLockStatus_t;

void   ServiceLock_Init(void);
bool   ServiceLock_TryUnlock(const char *token);
void   ServiceLock_Lock(void);
bool   ServiceLock_IsUnlocked(void);
void   ServiceLock_GetStatus(ServiceLockStatus_t *out);
bool   ServiceLock_CheckCommand(const char *cmd, bool requireUnlock);
uint32_t ServiceLock_GetBlockedCount(void);

#endif
```

**`service_lock.c` — Anahtar detaylar:**
```c
static bool     s_unlocked;
static uint32_t s_unlockTick;
static uint32_t s_blockedCount;

bool ServiceLock_CheckCommand(const char *cmd, bool requireUnlock)
{
    /* requireUnlock=true olan komutlar (base/boost/pi/gatetest/...)
     * servis kilidi açık değilse bloklanır ve sayaç artar. */
    if (requireUnlock && !s_unlocked)
    {
        s_blockedCount++;
        return false;
    }
    return true;
}
```

**`isDangerousServiceCmd()` — F446'dan port edilecek liste:**
```c
static const char *dangerousExact[] = {
    "gatetest", "identify", "test", "scan",
    "savecfg", "loadcfg", "saveall", "save",
    "map set", "map apply", "map reset",
    "map save", "map load", "map edit", "map discard",
    NULL
};
static const char *dangerousPrefixes[] = {
    "gatetest ", "base ", "boost ", "pi ",
    "kp ", "ki ", "kickduty ", "kickms ",
    "ramprate ", "rampms ", "defpwm ",
    NULL
};
```

**Kullanım senaryosu:**
```
> bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY
OK|service unlocked for 30s

> FL identify          → gönderilir (service unlock var)
> FL base 40 40 ...    → gönderilir (service unlock var)

> bridge lock_service
OK|service locked

> FL base 40 40 ...    → bloklanır, "ERR|service locked" döner
```

**Değişiklik yapılmayacak dosyalar:** Yok (yeni modül).

---

### Aşama 2: İki Aşamalı Stop + Estop (`motion_controller.c`)

**Amaç:** F446'daki `normalStopAll()`, `coastStopAll()`, `estop`
yapılarını H7'nin `motion_controller.c`'sine eklemek.

**Değişecek dosyalar:**
- `Core/Inc/motion_controller.h`
- `Core/Src/motion_controller.c`

**`motion_controller.h`'a eklenecekler:**
```c
/* Stop tipleri */
typedef enum {
    STOP_NORMAL,    /* rpm 0 → 15ms → stop (rampa) */
    STOP_COAST,     /* safe → 15ms → stop (coast) */
    STOP_ESTOP      /* direkt estop (acil durum) */
} StopType_t;

/* Yeni fonksiyonlar */
void MotionController_StopNormal(void);   /* önce rpm 0, gecikmeli stop */
void MotionController_StopCoast(void);    /* önce safe, gecikmeli stop */
void MotionController_StopEstop(void);    /* direkt estop */
void MotionController_ServiceDelayedStop(void);  /* loop'da çağrılacak */
bool MotionController_IsStopPending(void);
```

**`motion_controller.c` — Stop mantığı:**
```c
/* — Private state — */
static bool     s_delayedStopPending;
static uint32_t s_delayedStopDeadline;
#define SAFE_STOP_DELAY_MS  15u

void MotionController_StopNormal(void)
{
    /* 1. Önce rpm 0 (rampa) */
    char frame[16];
    int n = snprintf(frame, sizeof(frame), "%s\r\n", "rpm 0");
    if (n > 0)
        MotorDispatcher_SendRaw("rpm 0");

    /* 2. Gecikmeli stop planla */
    s_delayedStopPending = true;
    s_delayedStopDeadline = HAL_GetTick() + SAFE_STOP_DELAY_MS;
}

void MotionController_StopCoast(void)
{
    MotorDispatcher_SendRaw("safe");
    s_delayedStopPending = true;
    s_delayedStopDeadline = HAL_GetTick() + SAFE_STOP_DELAY_MS;
}

void MotionController_StopEstop(void)
{
    s_delayedStopPending = false;
    MotorDispatcher_SendRaw("estop");
}

void MotionController_ServiceDelayedStop(void)
{
    if (!s_delayedStopPending)
        return;
    if ((int32_t)(HAL_GetTick() - s_delayedStopDeadline) < 0)
        return;
    s_delayedStopPending = false;
    MotorDispatcher_SendRaw("stop");
    MotionController_Stop();  /* yerel motor tablosunu da sıfırla */
}
```

**`app_main.c`'de `App_Update()`'e eklenecek:**
```c
void App_Update(void)
{
    /* ... mevcut kod ... */
    MotionController_ServiceDelayedStop();  /* YENİ */
}
```

---

### Aşama 3: Yeni Komut Tipleri (`terminal_parser.c/h`)

**Amaç:** `estop`, `bridge`, `safe`/`alloff` komutlarını parser'a eklemek.

**Değişecek dosyalar:**
- `Core/Inc/terminal_parser.h` — yeni enum değerleri
- `Core/Src/terminal_parser.c` — yeni parse dalları

**`terminal_parser.h` — Yeni `TerminalCommandType_t` değerleri:**
```c
typedef enum {
    TCMD_NONE = 0,
    TCMD_HELP,
    TCMD_STOP,
    TCMD_BRAKE,
    TCMD_ESTOP,         /* YENİ: estop (acil durum) */
    TCMD_SAFE,          /* YENİ: safe/alloff (coast stop) */
    TCMD_IDENTIFY,
    TCMD_STATUS,
    TCMD_BRIDGE,        /* YENİ: bridge on/off/status/unlock/lock */
    TCMD_MODE_RPM,
    TCMD_MODE_PWM,
    TCMD_MODE_QUERY,
    TCMD_OP_MODE,
    TCMD_MOTION,
    TCMD_MOTOR_RAW,
    TCMD_MOTOR_TUNE
} TerminalCommandType_t;
```

**`terminal_parser.h` — Yeni alanlar:**
```c
typedef struct {
    TerminalCommandType_t type;
    MotionCmd_t   motion;
    RoverMode_t   opMode;
    bool          isDuty;
    uint16_t      value;
    uint16_t      originalValue;
    bool          hasValue;
    bool          wasClamped;

    MotorId_t     rawMotor;
    char          rawPayload[RAW_PAYLOAD_MAX];

    TuneMotorTarget_t tuneTarget;
    TuneCmdKind_t     tuneKind;
    char              tunePayload[TUNE_PAYLOAD_MAX];

    /* YENİ: bridge alt-komutları */
    uint8_t       bridgeAction;   /* 0=on, 1=off, 2=status, 3=unlock, 4=lock */
    char          bridgeToken[32]; /* unlock token'ı */
} TerminalCommand_t;
```

**`terminal_parser.c` — Yeni parse dalları (ekleme sırası):**

```c
/* stop'dan sonra: */
if (strcmp(buf, "estop") == 0)
{
    outResult->type = TCMD_ESTOP;
    return true;
}

if (strcmp(buf, "safe") == 0 || strcmp(buf, "alloff") == 0)
{
    outResult->type = TCMD_SAFE;
    return true;
}

/* bridge komutları — help'ten sonra, mode'dan önce: */
if (strncmp(buf, "bridge", 6) == 0)
{
    outResult->type = TCMD_BRIDGE;
    // buf+6'dan sonrasını parse et:
    // " on" → action=0
    // " off" → action=1
    // " status" → action=2
    // " unlock_service <token>" → action=3, token'ı kopyala
    // " lock_service" → action=4
    // geçersiz → return false
    return true;
}
```

---

### Aşama 4: Komut Yönlendirme (`command_handler.c`)

**Amaç:** Yeni komut tiplerini işleme sokmak, servis kilidi entegrasyonu,
DISARM gate'ini yeni komutlarla güncellemek.

**Değişecek dosyalar:**
- `Core/Src/command_handler.c`
- `Core/Inc/command_handler.h` (gerekirse)

**Önemli değişiklikler:**

**a) DISARM gate güncellemesi** (`CommandHandler_Handle`'de):
```c
if (OperatingMode_IsDisarm())
{
    bool allowed = false;
    switch (cmd->type)
    {
        case TCMD_OP_MODE:
        case TCMD_HELP:
        case TCMD_STATUS:
        case TCMD_MODE_QUERY:
        case TCMD_STOP:
        case TCMD_ESTOP:         /* YENİ: acil durum — DISARM'da izin ver */
        case TCMD_SAFE:          /* YENİ: coast stop — DISARM'da izin ver */
        case TCMD_BRAKE:
        case TCMD_BRIDGE:        /* YENİ: bridge yönetimi — DISARM'da izin ver */
        case TCMD_MOTOR_TUNE:
            allowed = true;
            break;
        /* ... TCMD_MOTOR_RAW için IsSafeRawPayload() aynı kalır ... */
    }
}
```

**b) `TCMD_ESTOP` handler'ı:**
```c
case TCMD_ESTOP:
    Logger_Log(LOG_WARN, "[ESTOP] Emergency stop");
    MotorTxDma_CancelPending();
    MotionController_StopEstop();
    ServiceLock_Lock();
    ActivityLight_SetMode(ROVER_MODE_DISARM);  /* LED kırmızı */
    break;
```

**c) `TCMD_SAFE` handler'ı:**
```c
case TCMD_SAFE:
    Logger_Log(LOG_INFO, "[SAFE] Coast stop");
    MotionController_StopCoast();
    break;
```

**d) `TCMD_BRIDGE` handler'ı:**
```c
case TCMD_BRIDGE:
    switch (cmd->bridgeAction)
    {
        case 0: /* bridge on */
            Logger_Log(LOG_INFO, "[BRIDGE] Telemetry forwarding enabled");
            // TelemetryForward_SetEnabled(true);
            break;
        case 1: /* bridge off */
            Logger_Log(LOG_INFO, "[BRIDGE] Telemetry forwarding disabled");
            break;
        case 2: /* bridge status */
        {
            ServiceLockStatus_t sls;
            ServiceLock_GetStatus(&sls);
            Logger_Log(LOG_INFO, "Bridge: telemetry=%s",
                       TelemetryForward_IsEnabled() ? "ON" : "OFF");
            Logger_Log(LOG_INFO, "Service: %s%s",
                       sls.unlocked ? "UNLOCKED" : "LOCKED",
                       sls.unlocked ? " (remaining " + sls.remaining_ms + "ms)" : "");
            Logger_Log(LOG_INFO, "Blocked commands: %lu", sls.blocked_count);
            break;
        }
        case 3: /* bridge unlock_service */
            if (ServiceLock_TryUnlock(cmd->bridgeToken))
                Logger_Log(LOG_INFO, "[BRIDGE] Service unlocked for 30s");
            else
                Logger_Log(LOG_ERROR, "[BRIDGE] Invalid unlock token");
            break;
        case 4: /* bridge lock_service */
            ServiceLock_Lock();
            Logger_Log(LOG_INFO, "[BRIDGE] Service locked");
            break;
    }
    break;
```

**e) Mevcut `TCMD_STOP` handler'ı güncellemesi:**
```c
case TCMD_STOP:
    MotionController_StopNormal();  /* eskiden direkt stop'tu */
    break;
```

**f) `TCMD_IDENTIFY` ve RAW komutlara servis kilidi eklenmesi:**

```c
case TCMD_IDENTIFY:
    if (!ServiceLock_IsUnlocked())
    {
        Logger_Log(LOG_ERROR, "[IDENTIFY] Service locked. Use: bridge unlock_service");
        break;
    }
    /* mevcut arm + identify mantığı aynen kalır */
    MotorDispatcher_SendRaw("arm service " SERVICE_TOKEN);
    if (!WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS))
    { /* hata yönetimi */ }
    MotorDispatcher_SendRaw("identify");
    break;

case TCMD_MOTOR_RAW:
    /* DISARM gate zaten geçildiyse, servis kilidini de kontrol et */
    if (IsDangerousRawPayload(cmd->rawPayload) && !ServiceLock_IsUnlocked())
    {
        Logger_Log(LOG_ERROR, "[SERVICE] Command blocked (service locked): %s",
                   cmd->rawPayload);
        ServiceLock_IncrementBlocked();
        break;
    }
    /* mevcut dispatch mantığı */
    break;
```

**g) `IsDangerousRawPayload()` — F446'daki `isDangerousServiceCmd()`'in H7 versiyonu:**
```c
static bool IsDangerousRawPayload(const char *p)
{
    if (p == NULL) return false;
    /* F446 listesindeki tehlikeli komutlar */
    static const char *dangerous[] = {
        "gatetest", "identify", "test", "scan",
        "savecfg", "loadcfg", "saveall", "save",
        "map set", "map apply", "map reset",
        "map save", "map load", "map edit", "map discard",
        NULL
    };
    /* ... F446'daki prefix mantığı aynen ... */
    return false;
}
```

---

### Aşama 5: Bridge Telemetri Kontrolü (`motor_uart_dma.c`)

**Amaç:** F446'daki `bridge on/off` mantığını H7'ye taşımak —
motorlardan gelen telemetrinin terminal'e yönlendirilmesini açıp
kapatabilme.

**Değişecek dosyalar:**
- `Core/Inc/motor_uart_dma.h` — `TelemetryForward_SetEnabled()` + `TelemetryForward_IsEnabled()`
- `Core/Src/motor_uart_dma.c` — yönlendirme kontrolü

**API:**
```c
/* Core/Inc/motor_uart_dma.h */
void TelemetryForward_SetEnabled(bool enabled);
bool TelemetryForward_IsEnabled(void);
```

**Kullanım:**
```c
// motor_uart_dma.c — RX mesajı işlendiğinde:
void MotorUartDma_ProcessRx(MotorId_t id, const char *msg)
{
    (void)id;
    if (TelemetryForward_IsEnabled())
    {
        Logger_Log(LOG_INFO, "%s|%s", MotorTagName(id), msg);
    }
}
```

Bu sayede `bridge off` komutu terminal'i spam'den korur ama iç
loglama (ACK takibi, link-loss tespiti) çalışmaya devam eder.

---

## 3. Dosya Değişiklik Özeti

| Dosya | Değişiklik |
|-------|-----------|
| **YENİ** `Core/Inc/service_lock.h` | Servis kilidi API'si |
| **YENİ** `Core/Src/service_lock.c` | Servis kilidi implementasyonu |
| `Core/Inc/terminal_parser.h` | Yeni enum değerleri (`TCMD_ESTOP`, `TCMD_SAFE`, `TCMD_BRIDGE`), yeni struct alanları |
| `Core/Src/terminal_parser.c` | Yeni parse dalları (estop, safe, bridge) |
| `Core/Inc/motion_controller.h` | Yeni stop fonksiyonları + `StopType_t` enum |
| `Core/Src/motion_controller.c` | İki aşamalı stop, estop, gecikmeli stop state machine |
| `Core/Src/command_handler.c` | DISARM gate güncellemesi, yeni handler'lar, servis kilidi entegrasyonu |
| `Core/Src/app_main.c` | `App_Update()`'e `MotionController_ServiceDelayedStop()` çağrısı |
| `Core/Inc/motor_uart_dma.h` | `TelemetryForward_SetEnabled()` + `IsEnabled()` |
| `Core/Src/motor_uart_dma.c` | Telemetri yönlendirme kontrolü |

---

## 4. Build ve Test

```bash
# Her aşama sonrası:
pio run -d h7-main 2>/dev/null && echo "BUILD OK" || echo "BUILD FAILED"

# Python araçları:
python -m py_compile tools/terminal.py

# Manuel test (terminal üzerinden):
#   bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY
#   FL identify
#   bridge lock_service
#   FL base 40 40 ...   -> bloklanmalı
#   stop                -> önce rpm 0, 15ms sonra stop
#   estop               -> direkt estop
```

---

## 5. Değişiklik Sırası ve Bağımlılıklar

```
Aşama 1 (service_lock)     ← bağımsız, ilk yapılır
    │
Aşama 2 (stop/estop)       ← bağımsız, service_lock'dan bağımsız
    │
Aşama 3 (parser)           ← Aşama 1 ve 2'ye bağımlı değil, paralel yapılabilir
    │
Aşama 4 (command_handler)  ← Aşama 1, 2, 3'ün TÜMÜNE bağımlı
    │
Aşama 5 (bridge telemetri) ← bağımsız, en son veya paralel
```

**Önerilen uygulama sırası:** 1 → 2 → 3 → 4 → 5

---

## 6. Geriye Dönük Uyumluluk

- Tüm mevcut komutlar (`f100`, `b50`, `mode disarm`, `FL status`, vb.)
  aynen çalışmaya devam eder.
- Yeni komutlar mevcut davranışı değiştirmez, sadece **ek** özellik sunar.
- `stop` komutunun davranışı değişir: artık direkt stop yerine
  `rpm 0` + gecikmeli `stop` gönderir. Bu bir iyileştirmedir,
  mevcut güvenlik konseptiyle uyumludur.
- Eğer eski stop davranışı istenirse, `safe` komutu (coast stop)
  kullanılabilir.

---

## 7. Riskler ve Dikkat Edilecekler

1. **İki aşamalı stop zamanlaması:** 15ms gecikme, `HAL_GetTick()`'in
   1ms çözünürlüğüyle uyumludur. Main loop gecikmesi 15ms'yi aşarsa
   stop gecikebilir — kritik değil, stop zaten garantili.

2. **Servis kilidi ve DISARM etkileşimi:** DISARM aktifken servis
   kilidi kontrolü yapılmaz (DISARM zaten her şeyi bloklar).
   Servis kilidi sadece MANUAL/AUTONOMOUS modlarında aktiftir.

3. **`TCMD_ESTOP`'un DISARM gate'deki yeri:** Estop, DISARM
   modunda bile izin verilmelidir (güvenlik gereği).

4. **`TelemetryForward_SetEnabled(false)`** durumunda link-loss
   tespiti ÇALIŞMAZ (`SafetyManager_NotifyRx()` çağrılmaz) —
   bu nedenle yönlendirme kontrolü sadece terminal log'lamasını
   etkilemeli, RX işleme zincirini durdurmamalıdır.
