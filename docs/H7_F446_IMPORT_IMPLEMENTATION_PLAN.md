# H7 F446 Import — Corrected Implementation Plan

> **Hedef:** `f446-bridge-test/` içindeki güvenlik ve kontrol yapılarını
> `h7-main/` rover controller mimarisine adapte etmek.
>
> **Prensip:** F446 kodu birebir kopyalanmayacak; konseptler H7'nin
> mevcut katmanlı yapısına adapte edilecek.
>
> **Orijinal plan:** `docs/F446_STRUCTURE_INTEGRATION_PLAN.md`
> Bu dosya, orijinal plandaki hataları düzeltip eksikleri tamamlayarak
> nihai uygulama planıdır.

---

## 0. Orijinal Plandaki Hatalar ve Düzeltmeler

### 0.1 C String Hatası (Kritik)

Orijinal planda şu tarz geçersiz C ifade kullanılmış:

```c
// YANLIŞ — C'de string + pointer birleştirmesi yok
sls.unlocked ? " (remaining " + sls.remaining_ms + "ms)" : ""
```

**Düzeltme:** `snprintf` ile ayrı buffer'a yaz veya iki ayrı
`Logger_Log` satırı kullan.

### 0.2 Eksik API Referansları

- **`ServiceLock_IncrementBlocked()`** — Orijinal planda çağrılıyor ama
  API'de tanımlı değil. **Düzeltme:** Blocked count artırımı
  `ServiceLock_CheckCommand()` ile yapılır; ayrı fonksiyona gerek yok.
- **`MotorTxDma_CancelPending()`** — H7'de zaten mevcut, sorun yok.

### 0.3 `identify` Akışı 4 Motorlu Hale Getirilmeli

Orijinal planda sadece global `TCMD_IDENTIFY` var. Eksik:
- `FL identify`, `FR identify`, `RL identify`, `RR identify`, `ALL identify`
- Her motor için H7 önce ilgili F411'e `arm service CURRENT_LIMITED_BENCH_SUPPLY`
  göndermeli, TX drain beklemeli, sonra `identify` göndermeli.
- Service unlock olmadan hiçbir identify/test/scan/gatetest çalışmamalı.

### 0.4 Service Lock Komutları Netleştirilmeli

Orijinal planda sadece `bridge unlock_service` var. Eksik:
- `service unlock CURRENT_LIMITED_BENCH_SUPPLY` alias
- `service lock` alias
- `service status` alias
- Ayrıca F446 uyumluluk için `bridge unlock_service` kalmalı.

### 0.5 Stop/Safe/Brake/Estop Sequence Overwrite Riski Gerçek

Orijinal plandaki tek `s_delayedStopPending` flag ile:
- `rpm 0 -> pending`, sonra `safe -> pending` = ilk komut overwritten.
- **Düzeltme:** Safety sequence — TX drain bekleyerek ikinci komutu gönder
  (mevcut `WaitForTxDrain` + `MotorTxDma_CancelPending` mekanizması).

### 0.6 `bridge off` RX Zincirini Kapatmamalı

Orijinal plan bunu belirtmiyor. **Düzeltme:** `bridge off` sadece
terminal forwarding'i kapatsın. RX işleme, SafetyManager_NotifyRx(),
ACK parsing ve link-loss tespiti ASLA kapanmamalı.

### 0.7 RX DMA Line Assembler Eksik

H7'nin `motor_uart_dma.c`'si DMA chunk'ı tam alır (`HAL_UARTEx_RxEventCallback`)
ama birden fazla satır tek chunk'ta gelebilir veya satır iki chunk arasında
bölünebilir. Mevcut kodda line assembler yok — düzeltilecek.

### 0.8 DISARM Gate ile Service Lock Etkileşimi

Orijinal plan: DISARM aktifken `identify` sadece service unlock ile çalışabilir.
Ama `IsSafeRawPayload()` fonksiyonu `identify`'yi "safe" olarak kabul ediyor
— bu yanlış. **Düzeltme:** `identify` artık dangerous kabul edilmeli,
service unlock olmadan engellenmeli.

### 0.9 Parser Limitleri F411 İle Hizalanmamış

- `kickms` üst limiti H7'de 10000, F411'de 1000.
- `boost` MS üst limiti H7'de 10000, F411'de 1000.
- `telper` minimum 1, maksimum 60000 — F411'de 20..5000 olmalı.
- RPM limiti: rover hareketinde güvenli 200, raw motorda F411 limiti 500.

### 0.10 Build Sistemi: Mutlak Path Sorunu

`h7-main/Debug/makefile` satır 63'te:
```
/home/garth/H7-DMA/STM32H723ZGTX_FLASH.ld
```
Bu mutlak path başka sistemde build'i engeller. Düzeltilecek.

---

## 1. Mevcut Durum Analizi (Güncellenmiş)

### H7'de Zaten Var Olan Karşılıklar

| F446 Yapısı | H7 Karşılığı | Durum |
|-------------|-------------|-------|
| `isValidFDuty()` / `isValidBDuty()` | `terminal_parser.c`'de `allDigits()` + `FillMotion()` | Mevcut |
| `isDirectPassthrough()` | Parser komut eşleştirme mantığı | Mevcut |
| `isDangerousServiceCmd()` | **Yok** — eklenecek | EKSİK |
| Servis kilidi (30s timeout) | **Yok** — eklenecek | EKSİK |
| İki aşamalı stop | Kısmen: DISARM'da `x`+stop var, ama genel `stop`/`safe` yok | EKSİK |
| `estop` | **Yok** — eklenecek | EKSİK |
| `bridge on/off/status` | **Yok** — eklenecek | EKSİK |
| `safe` / `alloff` | **Yok** — eklenecek | EKSİK |
| Per-motor service gate | **Yok** — eklenecek | EKSİK |
| RX DMA line assembler | **Yok** — eklenecek | EKSİK |

### H7'nin Mevcut Mimarisi (Dokunulmayacak)

```
terminal_if.c -> terminal_parser.c -> command_handler.c
                |                        |
                |          operating_mode.c (DISARM gate)
                |          safety_manager.c (link-loss, disarm helpers)
                |          control_mode.c (RPM/PWM)
                |
                v
         motion_controller.c -> motor_dispatcher.c -> motor_tx_dma.c
                                                         |
                                                         v
                                                  motor_uart_dma.c
```

**Dokunma kuralı:** `motor_dispatcher.c` altı (DISARM gate) korunacak.
Yeni katmanlar üstüne eklenecek.

---

## 2. Uygulama Aşamaları

Her aşama bağımsız commit. Sıra önemli.

---

### Aşama 0: Build Sistemi Düzeltmesi

**Amaç:** Mutlak path sorununu çöz.

**Dosyalar:**
- `h7-main/Debug/makefile` — linker script referansını göreceli yap
- `h7-main/STM32H723ZGTX_FLASH.ld` — dosya konumunu doğrula

**Değişiklik:**
```makefile
# ÖNCE (satır 63):
H7-DMA.elf H7-DMA.map: $(OBJS) ... /home/garth/H7-DMA/STM32H723ZGTX_FLASH.ld ...
# SONRA:
H7-DMA.elf H7-DMA.map: $(OBJS) ... ../STM32H723ZGTX_FLASH.ld ...
```

**Doğrulama:** `cd h7-main/Debug && make clean && make`

**Risk:** STM32CubeIDE auto-generate makefile'ı overwrite edebilir.
Geliştiriciya bilgi verilecek; IDE rebuild sonrası tekrar düzeltmek gerekebilir.

---

### Aşama 1: Servis Kilidi Modülü (`service_lock.c/h`)

**Yeni dosyalar:**
```
h7-main/Core/Inc/service_lock.h
h7-main/Core/Src/service_lock.c
```

**`service_lock.h` — Public API:**
```c
#ifndef SERVICE_LOCK_H
#define SERVICE_LOCK_H

#include <stdbool.h>
#include <stdint.h>

#define SERVICE_TIMEOUT_MS  30000u
#define SERVICE_TOKEN       "CURRENT_LIMITED_BENCH_SUPPLY"

typedef struct {
    bool     unlocked;
    uint32_t remaining_ms;   /* 0 if locked */
    uint32_t blocked_count;  /* lifetime counter */
} ServiceLockStatus_t;

void   ServiceLock_Init(void);
bool   ServiceLock_TryUnlock(const char *token);
void   ServiceLock_Lock(void);
bool   ServiceLock_IsUnlocked(void);
void   ServiceLock_Update(void);          /* auto-expire check (call from main loop) */
void   ServiceLock_GetStatus(ServiceLockStatus_t *out);

/* Check if a command requires service unlock.
 * If requireUnlock=true and service is locked:
 *   - blocked_count is incremented
 *   - returns false (command blocked)
 * If requireUnlock=true and service is unlocked:
 *   - returns true (command allowed)
 * If requireUnlock=false:
 *   - always returns true */
bool   ServiceLock_CheckCommand(const char *cmd, bool requireUnlock);

/* Dangerous command classification */
bool   ServiceLock_IsDangerousCmd(const char *cmd);

#endif
```

**`service_lock.c` — Anahtar detaylar:**
- `ServiceLock_Update()` her main-loop iterasyonda çağrılır, 30s expiry kontrolü yapar
- `ServiceLock_TryUnlock()` token karşılaştırması yapar (case-insensitive)
- `ServiceLock_CheckCommand()` — blocked_count bu fonksiyon içinden artar
- `ServiceLock_IsDangerousCmd()` — F446'nin `isDangerousServiceCmd()` mantığı:

```c
/* Dangerous exact matches (and prefix matches with trailing space) */
static const char *dangerousExact[] = {
    "gatetest", "identify", "test", "scan",
    "savecfg", "loadcfg", "saveall", "save",
    "map set", "map apply", "map reset",
    "map save", "map load", "map edit", "map discard",
    NULL
};
/* Dangerous prefix matches */
static const char *dangerousPrefixes[] = {
    "gatetest ", "base ", "boost ", "pi ",
    "kp ", "ki ", "kickduty ", "kickms ",
    "ramprate ", "rampms ", "defpwm ",
    "ramp ",     /* "ramp UP DOWN" — sets ramp rates */
    NULL
};
```

**Not:** `ServiceLock_IsDangerousCmd()` hem parser'da hem command_handler'da
kullanılacak. Parser'da TCMD_MOTOR_RAW için gate, command_handler'da
TCMD_IDENTIFY ve TCMD_MOTOR_TUNE için gate.

**Build doğrulama:** `cd h7-main/Debug && make`

---

### Aşama 2: Parser Genişletmesi — Yeni Komut Tipleri

**Değişecek dosyalar:**
- `h7-main/Core/Inc/terminal_parser.h`
- `h7-main/Core/Src/terminal_parser.c`

**`terminal_parser.h`'a eklenecek enum değerleri:**
```c
typedef enum {
    TCMD_NONE = 0,
    TCMD_HELP,
    TCMD_STOP,
    TCMD_BRAKE,
    TCMD_ESTOP,         /* YENİ */
    TCMD_SAFE,          /* YENİ: safe/alloff (coast stop) */
    TCMD_IDENTIFY,
    TCMD_STATUS,
    TCMD_BRIDGE,        /* YENİ: bridge on/off/status/unlock/lock */
    TCMD_SERVICE,       /* YENİ: service unlock/lock/status */
    TCMD_MODE_RPM,
    TCMD_MODE_PWM,
    TCMD_MODE_QUERY,
    TCMD_OP_MODE,
    TCMD_MOTION,
    TCMD_MOTOR_RAW,
    TCMD_MOTOR_TUNE
} TerminalCommandType_t;
```

**`terminal_parser.h`'a eklenecek alanlar:**
```c
typedef enum {
    BRIDGE_ON = 0,
    BRIDGE_OFF,
    BRIDGE_STATUS,
    BRIDGE_UNLOCK_SERVICE,
    BRIDGE_LOCK_SERVICE
} BridgeAction_t;

typedef enum {
    SERVICE_UNLOCK = 0,
    SERVICE_LOCK,
    SERVICE_STATUS
} ServiceAction_t;

/* TerminalCommand_t'e eklenecek alanlar: */
BridgeAction_t  bridgeAction;   /* TCMD_BRIDGE için */
ServiceAction_t serviceAction;  /* TCMD_SERVICE için */
char            serviceToken[RAW_PAYLOAD_MAX]; /* service unlock token */
```

**Parser'a eklenecek dallar (terminal_parser.c):**

| Komut | TCMD | Detay |
|-------|------|-------|
| `estop` | TCMD_ESTOP | Acil durum |
| `safe` / `alloff` | TCMD_SAFE | Coast stop |
| `bridge on` | TCMD_BRIDGE | bridgeAction=BRIDGE_ON |
| `bridge off` | TCMD_BRIDGE | bridgeAction=BRIDGE_OFF |
| `bridge status` | TCMD_BRIDGE | bridgeAction=BRIDGE_STATUS |
| `bridge unlock_service <token>` | TCMD_BRIDGE | bridgeAction=BRIDGE_UNLOCK_SERVICE, token sakla |
| `bridge lock_service` | TCMD_BRIDGE | bridgeAction=BRIDGE_LOCK_SERVICE |
| `service unlock <token>` | TCMD_SERVICE | serviceAction=SERVICE_UNLOCK, token sakla |
| `service lock` | TCMD_SERVICE | serviceAction=SERVICE_LOCK |
| `service status` | TCMD_SERVICE | serviceAction=SERVICE_STATUS |

**Per-motor identify desteği:**
- `FL identify`, `FR identify`, `RL identify`, `RR identify`:
  Parser bunları TCMD_MOTOR_RAW olarak parse eder (payload="identify").
  Command_handler'da service lock gate'ine takılır.
- `ALL identify`: Parser bunu TCMD_IDENTIFY olarak parse eder.
  Command_handler'da service lock gate'ine takılır.

**Not:** `identify` artık `IsSafeRawPayload()`'dan ÇIKARILMALIDIR.
DISARM altında sadece `status`, `stop`, `x`, `mode speed`, `mode duty`
safe olarak kabul edilmeli; `identify` service-restricted olmalı.

**Build doğrulama:** `cd h7-main/Debug && make`

---

### Aşama 3: Stop/Safe/Brake/Estop Sequence

**Değişecek dosyalar:**
- `h7-main/Core/Inc/motion_controller.h`
- `h7-main/Core/Src/motion_controller.c`

**Tasarım:** Tek `s_delayedStopPending` flag yerine, TX drain mekanizması
kullanılacak (mevcut `WaitForTxDrain` + `MotorTxDma_CancelPending`).

**Stop tipleri:**

| Komut | H7 -> F411 Sırası | Açıklama |
|-------|-------------------|----------|
| `stop` (normal) | `rpm 0` → TX drain → `stop` | Rampa ile durma, fault yok |
| `safe` / `alloff` (coast) | `safe` → TX drain → `stop` | Coast durma, fault yok |
| `brake` | `x` → TX drain → `stop` | Active brake, sonra stop |
| `estop` | `estop` | Direkt acil durum, service lock iptal |

**`motion_controller.h`'a eklenecekler:**
```c
void MotionController_StopNormal(void);  /* rpm 0 -> TX drain -> stop */
void MotionController_StopCoast(void);   /* safe -> TX drain -> stop */
void MotionController_StopEstop(void);   /* estop direkt */
```

**Uygulama detayları:**

`MotionController_StopNormal()`:
1. `MotorTxDma_CancelPending()` — eski motion frame'leri temizle
2. `MotorDispatcher_SendRaw("rpm 0")` — rampa ile sıfıra
3. `WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS)` — TX drain bekle
4. `MotorDispatcher_SendRaw("stop")` — coast/release
5. `MotionController_Stop()` — yerel motor tablosunu sıfırla

`MotionController_StopCoast()`:
1. `MotorTxDma_CancelPending()`
2. `MotorDispatcher_SendRaw("safe")` — F411'e coast gönder
3. `WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS)`
4. `MotorDispatcher_SendRaw("stop")`
5. `MotionController_Stop()`

`MotionController_StopEstop()`:
1. `MotorTxDma_CancelPending()`
2. `MotorDispatcher_SendRaw("estop")` — direkt estop
3. `MotionController_Stop()`
4. `ServiceLock_Lock()` — service unlock'u iptal et

**Overwrite riski neden çözüldü:** `CancelPending` + `WaitForTxDrain`
sayesinde ikinci komut birinci komut TX tamamlandıktan sonra gider.
Eski pending slot overwrite sorunu oluşmaz.

**DISARM gate uyumu:** Estop her modda çalışmalı.
Command_handler'da TCMD_ESTOP DISARM gate'inden geçen komutlar
listesine eklenmeli. Safe de aynı şekilde.

**Build doğrulama:** `cd h7-main/Debug && make`

---

### Aşama 4: Command Handler Güncellemesi — Tüm Yeni Komutlar

**Değişecek dosyalar:**
- `h7-main/Core/Src/command_handler.c`
- `h7-main/Core/Inc/command_handler.h`

**DISARM gate güncellemesi:**

DISARM'da izin verilen komutlar:
- `TCMD_OP_MODE` (mode disarm/manual/auto)
- `TCMD_HELP`
- `TCMD_STATUS`
- `TCMD_MODE_QUERY`
- `TCMD_STOP`
- `TCMD_BRAKE`
- `TCMD_ESTOP` (YENİ — her modda çalışmalı)
- `TCMD_SAFE` (YENİ — güvenli)
- `TCMD_BRIDGE` (YENİ — sadece bridge status sorgusu izinli)
- `TCMD_SERVICE` (YENİ — service unlock/lock/status izinli)
- `TCMD_MOTOR_TUNE` (sadece sorgu amaçlı tuning; AMA service lock gerektirenler engellenmeli)
- `TCMD_MOTOR_RAW` — sadece güvenli payload'lar (status, hall, stop, x, mode speed, mode duty)

**`IsSafeRawPayload()` güncellemesi:** `identify` ÇIKARILMALI.

**Yeni komut işleme:**

```c
case TCMD_ESTOP:
    MotionController_StopEstop();
    break;

case TCMD_SAFE:
    MotionController_StopCoast();
    break;

case TCMD_BRIDGE:
    HandleBridgeCommand(cmd);
    break;

case TCMD_SERVICE:
    HandleServiceCommand(cmd);
    break;

case TCMD_IDENTIFY:
    /* Service lock gate */
    if (!ServiceLock_IsUnlocked()) {
        Logger_Log(LOG_ERROR, "identify blocked: service locked. "
            "Use: service unlock CURRENT_LIMITED_BENCH_SUPPLY");
        /* ServiceLock_CheckCommand ile blocked count artır */
        ServiceLock_CheckCommand("identify", true);
        break;
    }
    /* Per-motor identify: arm + identify to each motor with TX drain */
    HandleIdentifyAll();
    break;
```

**TCMD_MOTOR_RAW service lock gate:**
```c
case TCMD_MOTOR_RAW:
{
    if (cmd->rawPayload[0] == '\0') {
        /* usage error */
        break;
    }
    /* Service lock gate for dangerous commands */
    if (ServiceLock_IsDangerousCmd(cmd->rawPayload) &&
        !ServiceLock_IsUnlocked()) {
        ServiceLock_CheckCommand(cmd->rawPayload, true);
        Logger_Log(LOG_ERROR,
            "[SERVICE] %s blocked: service locked. "
            "Use: service unlock CURRENT_LIMITED_BENCH_SUPPLY",
            cmd->rawPayload);
        break;
    }
    /* Mevcut raw gönderme mantığı */
    ...
}
```

**TCMD_MOTOR_TUNE service lock gate:**
```c
case TCMD_MOTOR_TUNE:
{
    /* Tune komutlarının bir kısmı dangerous (base, boost, pi, kickduty,
     * kickms, ramp). Sadece telper güvenli. */
    bool requiresUnlock = (cmd->tuneKind != TUNE_KIND_TELPER);
    if (requiresUnlock && !ServiceLock_IsUnlocked()) {
        ServiceLock_CheckCommand(cmd->tunePayload, true);
        Logger_Log(LOG_ERROR,
            "[SERVICE] %s blocked: service locked", cmd->tunePayload);
        break;
    }
    /* Mevcut tune gönderme mantığı */
    ...
}
```

**Per-motor identify (`FL identify` vs global `identify`):**

Parser artık `FL identify`'yi `TCMD_MOTOR_RAW, rawPayload="identify"` olarak
parse eder. Command_handler'da:
1. Service lock kontrolü (yukarıda)
2. Service unlock varsa:
   a. H7 o motora `arm service CURRENT_LIMITED_BENCH_SUPPLY` gönder
   b. TX drain bekle
   c. O motora `identify` gönder

```c
if (strcmp(cmd->rawPayload, "identify") == 0) {
    /* Per-motor identify with arming */
    char frame[64];
    snprintf(frame, sizeof(frame),
             "arm service CURRENT_LIMITED_BENCH_SUPPLY");
    MotorDispatcher_SendRawToMotor(cmd->rawMotor, frame);
    if (!WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS)) {
        Logger_Log(LOG_ERROR, "[IDENTIFY] Arm TX drain timeout");
        break;
    }
    MotorDispatcher_SendRawToMotor(cmd->rawMotor, "identify");
    break;
}
```

**`HandleBridgeCommand()`:**
```c
static void HandleBridgeCommand(const TerminalCommand_t *cmd) {
    switch (cmd->bridgeAction) {
    case BRIDGE_ON:
        TelemetryBridge_SetEnabled(true);
        Logger_Log(LOG_INFO, "bridge=ON (telemetry forwarding enabled)");
        break;
    case BRIDGE_OFF:
        TelemetryBridge_SetEnabled(false);
        Logger_Log(LOG_INFO, "bridge=OFF (telemetry forwarding disabled; "
            "RX processing still active)");
        break;
    case BRIDGE_STATUS:
    {
        ServiceLockStatus_t sls;
        ServiceLock_GetStatus(&sls);
        Logger_Log(LOG_INFO, "bridge=%s service=%s",
            TelemetryBridge_IsEnabled() ? "ON" : "OFF",
            sls.unlocked ? "UNLOCKED" : "LOCKED");
        if (sls.unlocked && sls.remaining_ms > 0) {
            Logger_Log(LOG_INFO, "unlock_remain_ms=%lu blocked_cmds=%lu",
                (unsigned long)sls.remaining_ms,
                (unsigned long)sls.blocked_count);
        } else {
            Logger_Log(LOG_INFO, "blocked_cmds=%lu",
                (unsigned long)sls.blocked_count);
        }
        break;
    }
    case BRIDGE_UNLOCK_SERVICE:
        if (ServiceLock_TryUnlock(cmd->serviceToken)) {
            Logger_Log(LOG_INFO, "service unlocked for %us",
                (unsigned)(SERVICE_TIMEOUT_MS / 1000));
        } else {
            Logger_Log(LOG_ERROR, "usage: bridge unlock_service "
                "CURRENT_LIMITED_BENCH_SUPPLY");
        }
        break;
    case BRIDGE_LOCK_SERVICE:
        ServiceLock_Lock();
        Logger_Log(LOG_INFO, "service locked");
        break;
    }
}
```

**`HandleServiceCommand()`:** Alias'lar, aynı mantık:
```c
static void HandleServiceCommand(const TerminalCommand_t *cmd) {
    switch (cmd->serviceAction) {
    case SERVICE_UNLOCK:
        if (ServiceLock_TryUnlock(cmd->serviceToken)) {
            Logger_Log(LOG_INFO, "service unlocked for %us",
                (unsigned)(SERVICE_TIMEOUT_MS / 1000));
        } else {
            Logger_Log(LOG_ERROR, "usage: service unlock "
                "CURRENT_LIMITED_BENCH_SUPPLY");
        }
        break;
    case SERVICE_LOCK:
        ServiceLock_Lock();
        Logger_Log(LOG_INFO, "service locked");
        break;
    case SERVICE_STATUS:
    {
        ServiceLockStatus_t sls;
        ServiceLock_GetStatus(&sls);
        Logger_Log(LOG_INFO, "service=%s",
            sls.unlocked ? "UNLOCKED" : "LOCKED");
        if (sls.unlocked && sls.remaining_ms > 0) {
            Logger_Log(LOG_INFO, "unlock_remain_ms=%lu blocked_cmds=%lu",
                (unsigned long)sls.remaining_ms,
                (unsigned long)sls.blocked_count);
        } else {
            Logger_Log(LOG_INFO, "blocked_cmds=%lu",
                (unsigned long)sls.blocked_count);
        }
        break;
    }
    }
}
```

**Build doğrulama:** `cd h7-main/Debug && make`

---

### Aşama 5: Telemetry Forwarding State

**Yeni dosyalar:**
```
h7-main/Core/Inc/telemetry_bridge.h
h7-main/Core/Src/telemetry_bridge.c
```

**`telemetry_bridge.h`:**
```c
#ifndef TELEMETRY_BRIDGE_H
#define TELEMETRY_BRIDGE_H

#include <stdbool.h>

void  TelemetryBridge_Init(void);
void  TelemetryBridge_SetEnabled(bool enabled);
bool  TelemetryBridge_IsEnabled(void);

#endif
```

**`telemetry_bridge.c`:** Basit bool flag. Varsayılan: ON.

**`motor_uart_dma.c`'de kullanımı:**
```c
// Mevcut:
if (strstr(line, "RPM:") != NULL && ...) {
    Logger_Log(LOG_INFO, "[TEL][%s] %s", slotMotorTag[i], line);
}
// Değişiklik:
if (strstr(line, "RPM:") != NULL && ...) {
    if (TelemetryBridge_IsEnabled()) {
        Logger_Log(LOG_INFO, "[TEL][%s] %s", slotMotorTag[i], line);
    }
    /* RX processing, SafetyManager_NotifyRx, ACK parsing, link-loss
     * ALWAYS active regardless of bridge state. */
}
```

**ÖNEMLİ:** `bridge off` sadece `Logger_Log` ile terminal forwarding'i
durdurur. `SafetyManager_NotifyRx()`, ACK parsing, link-loss tespiti
ASLA durmaz. Motor UART RX zinciri tamamen bağımsız çalışır.

**Build doğrulama:** `cd h7-main/Debug && make`

---

### Aşama 6: RX DMA Line Assembler

**Değişecek dosyalar:**
- `h7-main/Core/Src/motor_uart_dma.c`
- `h7-main/Core/Inc/motor_uart_dma.h`

**Mevcut sorun:** `HAL_UARTEx_RxEventCallback` DMA chunk'ı bir bütün
olarak `rxSlot[idx].msg`'e kopyalar. Birden fazla satır (RPM + [OK])
tek chunk'ta gelebilir. Satır `\n` ile ayrılmış olabilir.

**Çözüm:** Her motor UART için bir line buffer + pozisyon tut.

```c
#define MOTOR_LINE_BUF_SIZE  160

typedef struct {
    char    buf[MOTOR_LINE_BUF_SIZE];
    uint16_t pos;
    bool    overflow;
} MotorLineAssembler_t;

static MotorLineAssembler_t lineAsm[NUM_MOTOR_UARTS];
```

**`HAL_UARTEx_RxEventCallback`'de:**
Chunk'ı byte-byte tara. `\n` gelince tam satır `rxSlot`'a kopyala
ve `ready = true` yap. `\r` atla. Satır iki DMA chunk arasında
bölünmüşse assembler buffer'da birikir.

```c
/* Assembler: her byte için */
for (uint16_t i = 0; i < Size; i++) {
    uint8_t c = diag[idx].dmaBuf[i];
    if (c == '\r') continue;
    if (c == '\n') {
        if (lineAsm[idx].pos > 0 && !lineAsm[idx].overflow) {
            lineAsm[idx].buf[lineAsm[idx].pos] = '\0';
            uint16_t copyLen = lineAsm[idx].pos;
            if (copyLen > MOTOR_DMA_RX_MSG_MAX)
                copyLen = MOTOR_DMA_RX_MSG_MAX;
            memcpy(rxSlot[idx].msg, lineAsm[idx].buf, copyLen);
            rxSlot[idx].msg[copyLen] = '\0';
            rxSlot[idx].size = copyLen;
            rxSlot[idx].ready = true;
        }
        lineAsm[idx].pos = 0;
        lineAsm[idx].overflow = false;
        continue;
    }
    if (!lineAsm[idx].overflow) {
        if (lineAsm[idx].pos < MOTOR_LINE_BUF_SIZE - 1) {
            lineAsm[idx].buf[lineAsm[idx].pos++] = (char)c;
        } else {
            lineAsm[idx].overflow = true;
        }
    }
}
```

**Not:** ISR context'te `memcpy` kullanmak güvenli (deterministic, no
allocation). Logger_Log ISR'den çağrılmaz (sadece flag set eder).

**Build doğrulama:** `cd h7-main/Debug && make`

---

### Aşama 7: Parser Limitlerini F411 İle Hizala

**Değişecek dosyalar:**
- `h7-main/Core/Inc/app_config.h` — limit sabitleri ekle
- `h7-main/Core/Src/terminal_parser.c` — sabitleri kullan

**app_config.h'e eklenecekler:**
```c
/* ── F411-aligned parser limits ────────────────────────────────────────── */
#define H7_RPM_MAX_ROVER         200U    /* Rover motion: safe limit */
#define H7_RPM_MAX_RAW           500U    /* Raw motor command: F411 real limit */
#define H7_DUTY_MAX              4000U   /* Matches F411 PWM_MAX_DUTY */
#define H7_KICKMS_MAX            1000U   /* Matches F411 KICK_MS_MAX */
#define H7_BOOST_MS_MAX          1000U   /* Matches F411 — boost <8 PWMs> <ms> */
#define H7_TELPER_MIN            20U     /* Matches F411 TELEMETRY_INTERVAL_MS range */
#define H7_TELPER_MAX            5000U   /* Matches F411 practical range */
```

**terminal_parser.c'de değişiklikler:**
- `RPM_MAX` → `H7_RPM_MAX_ROVER` (rover hareket komutları için)
- Raw motor `rpm <signed>` komutları `H7_RPM_MAX_RAW` ile sınırlanacak
- `kickms` üst limit: `10000` → `H7_KICKMS_MAX = 1000`
- `boost` MS üst limit: `10000` → `H7_BOOST_MS_MAX = 1000`
- `telper` min/max: `1..60000` → `H7_TELPER_MIN..H7_TELPER_MAX`

**Build doğrulama:** `cd h7-main/Debug && make`

---

### Aşama 8: App Main ve Init Güncellemesi

**Değişecek dosyalar:**
- `h7-main/Core/Src/app_main.c`

**Eklenecek init çağrıları:**
```c
void App_Init(void) {
    /* ... mevcut init ... */
    ServiceLock_Init();          /* YENİ */
    TelemetryBridge_Init();     /* YENİ */
}

void App_Update(void) {
    /* ... mevcut kod ... */
    ServiceLock_Update();       /* YENİ: auto-expire service unlock */
    /* ... */
}
```

**Build doğrulama:** `cd h7-main/Debug && make`

---

### Aşama 9: Build Sistemi — Yeni Kaynak Dosyaları

**Değişecek dosyalar:**
- `h7-main/Debug/Core/Src/subdir.mk` — yeni .c dosyalarını ekle
- `h7-main/Debug/Core/Src/subdir.mk` — yeni .o hedeflerini ekle
- `h7-main/Debug/Core/Src/subdir.mk` — yeni .d bağımlılıklarını ekle

**Eklenecek dosyalar:**
- `service_lock.c` / `service_lock.o` / `service_lock.d`
- `telemetry_bridge.c` / `telemetry_bridge.o` / `telemetry_bridge.d`

**Build doğrulama:** `cd h7-main/Debug && make clean && make`

---

### Aşama 10: GUI — 4 Motorlu Test Panel

**Değişecek dosyalar:**
- `h7-main/earendil.py`

**Eklenecek:**
- Motor seçimi combobox: `FL | FR | RL | RR | ALL`
- Butonlar:
  - Status, Hall, Mode Duty, Mode Speed
  - Forward Duty Test, Backward Duty Test, RPM Test
  - Normal Stop, Safe Stop, Brake, E-stop
  - Identify, Service Unlock, Service Lock
  - Bridge On, Bridge Off, Bridge Status
- Riskli komutlar için onay penceresi:
  - "Motor unloaded? Current-limited bench PSU connected? E-stop accessible?"

**Buton -> Komut Eşlemesi:**

| Buton | Komut |
|-------|-------|
| Status | `FL status` (veya seçili motor) |
| Hall | `FL hall` |
| Mode Duty | `FL mode duty` |
| Mode Speed | `FL mode speed` |
| Forward Duty Test | `FL fd200` |
| Backward Duty Test | `FL bd200` |
| RPM Test | `FL f30` |
| Normal Stop | `stop` |
| Safe Stop | `safe` |
| Brake | `brake` |
| E-stop | `estop` |
| Identify | `FL identify` (onay penceresi ile) |
| Service Unlock | `service unlock CURRENT_LIMITED_BENCH_SUPPLY` |
| Service Lock | `service lock` |
| Bridge On | `bridge on` |
| Bridge Off | `bridge off` |
| Bridge Status | `bridge status` |

**Build doğrulama:** `python3 -m py_compile h7-main/earendil.py`

---

## 3. Dosya Bazlı Değişiklik Listesi

| Dosya | Aşama | Değişiklik Türü |
|-------|-------|----------------|
| `h7-main/Debug/makefile` | 0 | Linker script mutlak path → göreceli |
| `h7-main/Core/Inc/service_lock.h` | 1 | YENİ |
| `h7-main/Core/Src/service_lock.c` | 1 | YENİ |
| `h7-main/Core/Inc/terminal_parser.h` | 2 | Yeni enum + struct alanları |
| `h7-main/Core/Src/terminal_parser.c` | 2 | Yeni parse dalları |
| `h7-main/Core/Inc/motion_controller.h` | 3 | Yeni fonksiyonlar |
| `h7-main/Core/Src/motion_controller.c` | 3 | Stop normal/coast/estop |
| `h7-main/Core/Src/command_handler.c` | 4 | Tüm yeni komut handler'ları |
| `h7-main/Core/Inc/command_handler.h` | 4 | Yeni classification fonksiyonları |
| `h7-main/Core/Inc/telemetry_bridge.h` | 5 | YENİ |
| `h7-main/Core/Src/telemetry_bridge.c` | 5 | YENİ |
| `h7-main/Core/Src/motor_uart_dma.c` | 5+6 | Telemetry bridge + line assembler |
| `h7-main/Core/Inc/motor_uart_dma.h` | 6 | Line assembler sabitleri |
| `h7-main/Core/Inc/app_config.h` | 7 | F411-aligned limitler |
| `h7-main/Core/Src/terminal_parser.c` | 7 | Limit güncellemeleri |
| `h7-main/Core/Src/app_main.c` | 8 | Init/update çağrıları |
| `h7-main/Debug/Core/Src/subdir.mk` | 9 | Yeni kaynak dosyaları |
| `h7-main/earendil.py` | 10 | 4 motorlu test panel |

---

## 4. Riskler

| Risk | Etki | Azaltma |
|------|------|---------|
| STM32CubeIDE makefile'ı overwrite edebilir | Build kırılır | IDE rebuild sonrası tekrar düzelt; idealde proje .cproject bazlı build'e geç |
| ISR context'te line assembler stres testi eksik | Yavaş UART'da veri kaybı | Bench test: `ALL telper 20` ile 4 motor high-rate telemetry |
| Service lock 30s timeout yeterli mi? | Identify bitmeden expire | F411 identify ~20s; 30s yeterli ama sınırda |
| `WaitForTxDrain` 100ms bloklama | Main loop gecikmesi | Sadece stop/safe/estop sırasinda; normal motion'u etkilemez |
| `estop` service lock'u iptal eder | Kullanıcı tekrar unlock yapmalı | F446 davranışı ile uyumlu; beklenen |

---

## 5. Geri Dönüş Stratejisi

Her aşama bağımsız commit. Bir aşama başarısız olursa:
1. `git revert <commit-hash>` ile o aşamayı geri al
2. Sorunu düzelt, tekrar commit
3. Mevcut çalışan komutlar kırılmamalı — her commit sonrası test:
   `f100`, `b100`, `l100`, `r100`, `stop`, `mode disarm`, `mode manual`,
   `FL status`, `FR status`, `RL status`, `RR status`

---

## 6. Varsayımlar

1. H7 STM32CubeIDE Makefile tabanlı build kullanıyor (PlatformIO değil).
2. `WaitForTxDrain()` mevcut yapıda güvenli (main-loop context, ISR değil).
3. F411 firmware'de `arm service CURRENT_LIMITED_BENCH_SUPPLY` komutu
   zaten mevcut ve 30 saniyelik arm süresi ile çalışıyor.
4. H7 terminal baud: 115200, motor UART baud: 115200.
5. `MotorTxDma_CancelPending()` mevcut (doğrulandı).
