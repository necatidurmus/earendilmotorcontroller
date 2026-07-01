# H7 Firmware'e F446 Bridge Yapılarının Import Planı

> **Sürüm:** 1.0
> **Hedef:** F446'da test edilmiş tek-motor bridge/safety davranışlarını
> H7'nin 4 motorlu DMA/UART mimarisine güvenli ve kontrollü şekilde aktarmak.
>
> **Kapsam:** `h7-main/` (hedef) ← `f446-bridge-test/` (referans) ← `f411-motor-cube/` (protokol uyumluluğu)

---

## 1. Proje Mimarisi Özeti

### Üç Katmanlı Sistem

```
                    ┌──────────────────────────────────────────────┐
                    │              PC / Terminal / GUI             │
                    │          (earendil.py / Serial Monitor)      │
                    └──────────────────────┬───────────────────────┘
                                           │ USART3 @115200
                                           ▼
                    ┌──────────────────────────────────────────────┐
                    │          H7 ROVER MAIN CONTROLLER            │
                    │          (STM32H723ZG, NUCLEO-H723ZG)        │
                    │                                              │
                    │  USART2 ────► FL (Front Left  F411)          │
                    │  UART4  ────► FR (Front Right F411)          │
                    │  UART5  ────► RR (Rear Right  F411)          │
                    │  UART7  ────► RL (Rear Left   F411)          │
                    └──────┬──────┬──────┬──────┬──────────────────┘
                           │      │      │      │  UART @115200
                    ┌──────┘      │      │      └──────┐
                    ▼             ▼      ▼              ▼
               ┌────────┐  ┌────────┐ ┌────────┐  ┌────────┐
               │ F411   │  │ F411   │ │ F411   │  │ F411   │
               │ FL     │  │ FR     │ │ RL     │  │ RR     │
               │ Motor  │  │ Motor  │ │ Motor  │  │ Motor  │
               └────────┘  └────────┘ └────────┘  └────────┘
```

### Her Katmanın Sorumluluğu

| Katman | Rol | Motor kontrolü? | Hall sensör? | TIM1/PWM? | Güvenlik? |
|--------|-----|:---:|:---:|:---:|:---:|
| **F446 bridge** | PC ↔ F411 arasında test köprüsü, servis kilidi, stop/estop | Hayır | Hayır | Hayır | Evet (service unlock, stop sequences) |
| **H7 rover** | Komut yönlendirici, 4x F411 koordinatörü, DISARM güvenlik kilidi | Hayır | Hayır | Hayır | Evet (DISARM, link-loss, ACK, defense-in-depth) |
| **F411 motor** | Motor komütasyonu, hall sensör, PI kontrol, PWM üretimi | Evet | Evet | Evet (TIM1) | Evet (watchdog, fault, brake) |

### Veri/ Komut Akışı

```
Kullanıcı: "FL f100"

1. PC/USART3 → H7 terminal_if.c (interrupt RX)
2. terminal_parser.c (lowercase, trim, parse → TCMD_MOTOR_RAW)
3. command_handler.c (DISARM gate → service lock gate → dispatch)
4. motor_dispatcher.c → MotorTxDma_Send(MOTOR_FL, "f100\r\n")
5. USART2 DMA TX → FL F411 USART2 RX
6. F411 parser → "f100" → duty=100, MODE_DUTY → motor döner
7. F411 telemetry → USART2 TX → H7 USART2 DMA RX
8. motor_uart_dma.c → rxSlot[FL].ready = true
9. MotorUartDma_Update() → Logger_Log("[TEL][FL] RPM:...")
10. USART3 TX → PC/GUI'ye [TEL][FL] RPM:... satırı
```

---

## 2. Motor-UART Eşleme Doğrulaması

### Tanımlı Mapping (Tüm Kaynaklarda Tutarlı mı?)

| Motor | Enum | UART Periph | UART Handle | DMA RX Stream | DMA TX Stream |
|-------|------|-------------|-------------|---------------|---------------|
| **FL** | `MOTOR_FL=0` | **USART2** | `huart2` | Stream3 | Stream7 |
| **FR** | `MOTOR_FR=1` | **UART4**  | `huart4` | Stream0 | Stream4 |
| **RL** | `MOTOR_RL=2` | **UART7**  | `huart7` | Stream2 | Stream6 |
| **RR** | `MOTOR_RR=3` | **UART5**  | `huart5` | Stream1 | Stream5 |

### Tutarlılık Kontrolü

| Kaynak | Dosya | Satır | Tutarlı mı? |
|--------|-------|-------|:-----------:|
| Motor enum | `rover_types.h` | 8-15 | ✅ FL=0,FR=1,RL=2,RR=3 |
| UART handle macro | `app_config.h` | 29-34 | ✅ FL→huart2, FR→huart4, RL→huart7, RR→huart5 |
| TX DMA kanal tablosu | `motor_tx_dma.c` | 46-52 | ✅ Aynı sıra |
| RX DMA instance eşleme | `motor_uart_dma.c` | 63-68 | ✅ USART2=0, UART4=1, UART5=2, UART7=3 |
| RX motor tag | `motor_uart_dma.c` | 31 | **⚠️ RR/RL sırası farklı:** `slotMotorTag` = `{"FL","FR","RR","RL"}` — sadece label, index USART2=0, UART4=1, UART5=2, UART7=3 ile eşleşiyor |
| Safety manager eşleme | `motor_uart_dma.c` | 71-82 | ✅ USART2→FL, UART4→FR, UART7→RL, UART5→RR |
| GUI telemetry regex | `earendil.py` | 215-220 | ✅ `[TEL][FL/FR/RL/RR]` |
| CubeMX `.ioc` | `H7-DMA.ioc` | — | **Doğrulanmalı** (ikili dosya, manuel kontrol gerek) |

**Sonuç:** Tüm C kaynakları arasında FL=USART2, FR=UART4, RL=UART7, RR=UART5 eşleşmesi **tutarlı**. Sadece RX slot label dizisinde RR/RL sırası farklı ama bu sadece log etiketi — fonksiyonel etkisi yok.

---

## 3. F446'dan H7'ye Taşınması Gereken Davranışlar

### 3.1 Stop Davranışı

| Durum | F446 (referans) | H7 (mevcut) | Ne yapılmalı? |
|-------|----------------|-------------|---------------|
| **Normal stop** | `rpm 0` → 15ms → `stop` | Direkt `MotionController_Stop()` → stop | **H7 de iki aşamalı yapılmalı** — aynı F446'daki gibi |
| **Coast stop** | `safe` → 15ms → `stop` | Yok (`safe` komutu mevcut değil) | **Ekle** (güvenli coast) |
| **Brake** | `x`/`brake` → direkt F411'e | `MotorDispatcher_SendRaw("x")` | Mevcut yeterli, sadece brake sonrası `stop` eklenebilir |
| **E-stop** | Direkt `estop` → F411'e, fault latch clear | Yok | **Ekle** — DISARM'dan bağımsız acil durum |

**Kritik fark:** H7'nin mevcut `MotionController_Stop()` fonksiyonu (`motion_controller.c:72-77`) direkt `MCMD_STOP` + `MotorDispatcher_SendAll()` çağırıyor — yani `stop` komutunu F411'lere yolluyor. F446'daki iki aşamalı yaklaşım (önce `rpm 0` rampa, 15ms bekle, sonra `stop`) çok daha güvenli çünkü:
- Motor önce kontrollü yavaşlar (rpm 0 ramp)
- Sonra coast (stop) ile tamamen durur
- Ani coast akım gerilmesini önler

### 3.2 Safe/Coast Stop

**F446:** `safe` → F411'e `safe` gönderir → tüm gate'ler kapatılır (coast) → 15ms → `stop`

**H7:** Mevcut durumda `safe` komutu tanımlı değil. `command_handler.c` ve `terminal_parser.c`'ye eklenmeli.

### 3.3 Brake/x

**F446:** `x` veya `brake` → direkt F411'e `x` gönderir (aktif brake, tüm low-side MOSFET'ler ON)

**H7 (mevcut):** `TCMD_BRAKE` → `MotorDispatcher_SendRaw("x")` — yeterli. BRAKE_HOLD_MS timeout sonrası `rpm 0` + `stop` gönderme mantığı F411'de zaten var.

### 3.4 E-Stop

**F446:** `estop` → direkt F411'e `estop` → F411'de FAULT_ESTOP yükseltilir → sonraki motion komutu fault'u temizler

**H7 (mevcut):** Yok. Eklenmeli.

**Önerilen H7 davranışı:**
1. `MotorTxDma_CancelPending()` — tüm bekleyen komutları iptal et
2. `MotorDispatcher_SendRaw("estop")` — 4 F411'e de `estop` gönder
3. `ServiceLock_Lock()` — servis kilidini kapat
4. `OperatingMode_Set(DISARM)` — güvenli moda geç
5. Logger: `[ESTOP] Emergency stop sent`

### 3.5 Identify

**F446 akışı:**
```
PC: bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY
F446: OK|service unlocked for 30s
PC: m1 identify
F446: → F411'e "arm service CURRENT_LIMITED_BENCH_SUPPLY" (önce arm)
       → F411: [OK] Service armed for 30s
       → F446: TX|arm service...
       → F446: TX|identify
       → F411: identify sonucu
       → F446: M1|[OK] Identify updated RAM hall map
```

**H7 mevcut akışı** (`command_handler.c:362-378`):
```
PC: identify
H7: MotorDispatcher_SendRaw("arm service CURRENT_LIMITED_BENCH_SUPPLY")
H7: WaitForTxDrain(100ms)  ← TX drain bekle
H7: MotorDispatcher_SendRaw("identify")
```

**Sorun:** H7'de mevcut `identify` komutu:
- `FL identify` veya `FR identify` gibi **per-motor** değil — tüm motorlara `identify` gönderiyor
- Hiçbir **servis kilidi kontrolü** yapmıyor (direkt `arm service` + `identify` gönderiyor)
- DISARM modunda `identify` raw komut olarak izin veriliyor ama servis kilidi yok

**Yapılması gereken:**
- `FL identify`, `FR identify`, `RL identify`, `RR identify`, `ALL identify` ayrımı
- Her identify öncesi servis kilidi kontrolü
- `arm service CURRENT_LIMITED_BENCH_SUPPLY` + TX drain + `identify` akışı per-motor

### 3.6 Service Unlock

**F446:** `bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY` → 30sn timeout → tehlikeli komutlar bloke

**H7 (mevcut):** DISARM modu var. Ama F446'daki gibi 30sn token doğrulamalı bir servis kilidi **yok**. `identify` komutu direkt `arm service` gönderiyor.

**Öneri:** `command_handler.c`'deki mevcut `identify` işlemi öncesi servis kilidi eklensin. Ayrı `bridge unlock` komutu yerine `service unlock <token>` veya mevcut `arm service` yaklaşımı korunabilir.

### 3.7 Mode Speed/Duty

**F446:** `mode speed` / `mode duty` → direkt F411'e passthrough

**H7 (mevcut):** `m speed` / `m duty` veya `mode speed` / `mode duty`
1. Önce `stop` gönder (F411 "Stop motor first" hatası vermesin)
2. `WaitForTxDrain(100ms)`
3. `mode speed`/`mode duty` gönder
4. `ControlMode_Set()` ile lokal modu güncelle

Bu yapı F446'dan **daha iyi** — sıralı stop + drain mantığı doğru çalışıyor. Korunmalı.

### 3.8 Tuning Komutları

**H7 (mevcut):** `FL base P1..P8`, `FL boost P1..P8 MS`, `FL pi KP KI`, `FL ramp UP DOWN`, `FL kickduty N`, `FL kickms N`, `FL telper MS` — `terminal_parser.c` içinde tam validasyon var. `ParseTuneCommand()` fonksiyonu 8 farklı tuning komutunu ayrıştırıyor.

**F446'da olan H7'de olmayan tuning:**
- `kp <v>` / `ki <v>` (sadece `pi kp ki` var)
- `ramprate <n>` / `rampms <n>` (sadece `ramp UP DOWN` var — RPM/s formatında)
- `defpwm <n>` (default PWM)
- `kick on` / `kick off` / `ramp on` / `ramp off` (toggle komutları)

Bunlar düşük öncelikli — F446 testleri sırasında ihtiyaç duyulmadıysa eklenmeyebilir.

### 3.9 Telemetry Prefix'leme

**F446:** `M1|RPM:23,T:30,...` — motor adı prefix'iyle PC'ye yönlendirir

**H7 (mevcut):** `[TEL][FL] RPM:23,T:30,...` — `motor_uart_dma.c:250` zaten motor tag'i ile log'luyor. GUI regex'i (`earendil.py:215-220`) bu formatı parse ediyor.

**Durum:** H7'nin mevcut formatı F446'dan daha iyi — `[TEL][MOTOR]` prefix'iyle net ayrım var. GUI zaten destekliyor. **Değişiklik gerekmez.**

### 3.10 GUI Test Paneli Davranışı

**F446 GUI (tools/terminal.py veya benzeri):** Bilinmiyor — `f446-bridge-test/` içinde GUI yok.

**H7 GUI (earendil.py):** 2569 satırlık PySide6 uygulaması:
- 4 motorlu telemetry tablosu (FL/FR/RL/RR)
- Motor tuning dialog (`MotorSettingsDialog`)
- Serial reader thread
- UART error log parsing
- Operating mode indicator
- Klavye kısayolları (WASD, Space, X, M, I)

**GUI'de olmayıp F446 mantığından eklenmesi gerekenler:**
- `Service Unlock` butonu + token input
- `Service Lock` butonu
- Motor seçicili `Identify` butonu
- `Safe Stop` / `Coast Stop` butonu
- `E-Stop` butonu
- `Blocked command count` göstergesi

---

## 4. F446'dan Birebir Taşınmaması Gereken Parçalar

| Parça | Neden Taşınmamalı |
|-------|------------------|
| `HardwareSerial MotorSerial(MOTOR_RX_PIN, MOTOR_TX_PIN)` | Arduino API'si, H7 HAL kullanıyor |
| `Serial.begin()` / `Serial.available()` / `Serial.read()` | Arduino API'si, H7 DMA kullanıyor |
| `digitalWrite()` / `pinMode()` | Arduino API'si, H7 HAL_GPIO kullanıyor |
| Tek `M1` motor varsayımı | H7'de 4 motor var: FL, FR, RL, RR |
| F446 bridge prefix `M1\|` | H7 `[TEL][FL]` formatı daha iyi |
| `delay(300)` setup'da | H7'de delay kullanılmaz (HAL_Delay yasak) |
| `millis()` | H7 `HAL_GetTick()` kullanır |
| Karakter-karakter UART pump (`Serial.available()`) | H7 DMA RX kullanır (daha verimli) |
| `isDirectPassthrough()` listesi | H7'nin `terminal_parser.c`'si çok daha kapsamlı |
| `bridge on/off` (telemetri kontrolü) | H7'de tüm telemetri loglanır, bridge kavramı yok |

**Kural:** H7'nin DMA/UART mimarisini bozacak hiçbir Arduino/HAL dışı yapı taşınmamalı.

---

## 5. H7'de Korunması Gereken Mevcut Yapılar

| Yapı | Dosya | Neden Korunmalı |
|------|-------|-----------------|
| 4 motorlu dispatcher | `motor_dispatcher.c` | Çalışıyor, test edilmiş, defense-in-depth DISARM gate var |
| UART DMA TX (active + pending slot) | `motor_tx_dma.c` | Non-blocking, stop/brake prioritization (`IsSafetyCommand()`), CancelPending |
| UART DMA RX (ReceiveToIdle_DMA) | `motor_uart_dma.c` | Kesintisiz alım, error recovery, link-loss notification |
| Motor enum yapısı | `rover_types.h` | FL=0, FR=1, RL=2, RR=3 — tüm sistem tutarlı |
| Motion controller tank-drive | `motion_controller.c` | f/b/r/l tank steering mantığı doğru çalışıyor |
| Tuning komut validasyonu | `terminal_parser.c` | 8 farklı tuning komutu için tam validasyon |
| DISARM mode state machine | `operating_mode.c` | Rover-level güvenlik kilidi, defense-in-depth |
| Control mode switch (stop + drain) | `command_handler.c:137-184` | Sıralı geçiş, F411 "Stop motor first" hatasını engelliyor |
| Link-loss tespiti (3s timeout) | `safety_manager.c` | RX kesilirse uyarı verir |
| ACK timeout/retry | `ack_manager.c` | Gelecekte tam ACK doğrulaması için altyapı |
| GUI 4-motor telemetry tablosu | `earendil.py` | Motor başına telemetry görüntüleme |
| DMA buffer'ları RAM_D2'de | `linker script`, `motor_tx_dma.c:10`, `motor_uart_dma.c:10` | Non-cacheable — cache coherency sorununu önler |

---

## 6. Kritik Riskler

### Risk 1: `identify` DISARM Raw Allowlist İçinde Güvenli Kabul Ediliyor

`command_handler.c:43-44`: `IsSafeRawPayload()` içinde `identify` güvenli kabul ediliyor:
```c
return (strcmp(p, "status")     == 0 ||
        strcmp(p, "identify")  == 0 ||   // ← DISARM'da bile izin veriliyor
        strcmp(p, "stop")      == 0 ||
        ...);
```

**Risk:** DISARM modunda kullanıcı `FL identify` yazarsa, `arm service CURRENT_LIMITED_BENCH_SUPPLY` gönderilir (F411 arming olur) ve identify çalışır. Bu, DISARM'ın güvenlik amacını aşar — identify motoru döndürebilir.

**Çözüm:** `identify` ya DISARM allowlist'ten çıkarılmalı ya da servis kilidi kontrolü eklenmeli.

### Risk 2: Service Unlock Olmadan Tehlikeli Servis Komutları Çalışabilir

H7'de F446'daki gibi bir `isDangerousServiceCmd()` filtresi yok. `FL gatetest`, `FL test`, `FL save`, `FL map set` gibi komutlar direkt F411'e gider (DISARM değilse veya raw payload allowlist'ten geçiyorsa).

**Risk:** Kullanıcı farkında olmadan tehlikeli bir servis komutu gönderebilir.

**Çözüm:** H7'ye de F446'daki `isDangerousServiceCmd()` benzeri bir filtre + servis kilidi eklenmeli.

### Risk 3: Safety Komut Sıraları Overwrite Olabilir

`motor_tx_dma.c:162-169`: Pending slot mantığı — safety komut (stop/x) her zaman overwrite eder, normal komut safety'i overwrite etmez:
```c
if (newIsSafety || !ch->pendingSafety) {
    CopyCommand(ch->pendingBuffer, cmd, &ch->pendingLen);
    ...
}
```

**Risk:** `x` (brake) gönderildikten sonra, TX tamamlanmadan hemen `stop` gönderilirse `stop` pending slot'a yazılır. Ama önce `rpm 0` sonra `stop` gibi bir sıra gerekiyorsa, aradaki `rpm 0` kaybolabilir.

**Çözüm (mevcut):** `WaitForTxDrain()` kullanarak safety sıraları arasında drain beklenebilir. Ancak bu blocking bir approach. Alternatif olarak **multi-pending queue** veya **safety sequencer** eklenebilir ama bu büyük bir değişiklik.

**Tavsiye:** İki aşamalı stop (rpm 0 → 15ms → stop) için 15ms bekleme, TX drain için yeterli olacaktır. 15ms'de DMA TX çoktan tamamlanır (64 byte @115200 = ~5.5ms). Bu nedenle **mevcut tek pending slot yeterli**.

### Risk 4: RX DMA Satır Bazlı Telemetry Toplamıyor

`motor_uart_dma.c`'deki `HAL_UARTEx_RxEventCallback()` (`motor_uart_dma.c:262-306`):
- DMA RX buffer'ı dolduğunda veya Idle line tespit edildiğinde çağrılır
- Gelen tüm veriyi (`Size` byte) olduğu gibi `rxSlot[idx].msg`'e kopyalar
- **Satır bazlı bölme yok** — F411'den gelen birden fazla satır aynı anda gelebilir

**Risk:** Eğer F411 hızlı telemetry gönderiyorsa (örn. her 20ms'de bir), H7'nin 128-byte DMA buffer'ı dolmadan Idle line oluşmayabilir. Bu durumda birden fazla telemetry satırı tek bir mesajda birleşir ve parse edilemez.

**Çözüm:** RX işleme zincirine bir **line assembler** eklenmeli:
- Gelen ham veriyi `\n` bazında böl
- Tamamlanmamış satırı buffer'da tut
- Her tam satır için `rxSlot[idx].ready = true` yap

Bu, H7'nin mevcut yapısındaki **en önemli eksiklik**. F446'da `pumpMotorPort()` karakter-karakter okuyarak `\n` bazında bölüyordu. H7'nin DMA yaklaşımında da aynı işlevi görecek bir line assembler şart.

### Risk 5: H7 Parser Limitleri F411 Limitleriyle Uyumsuz

| Parametre | H7 Parser Limit | F411 Limit | Uyumlu mu? |
|-----------|----------------|------------|:----------:|
| RPM max | `RPM_MAX = 200` (`terminal_parser.c:8`) | `MAX_RPM_TARGET = 500` (`f411/app_config.h:150`) | **⚠️ H7 200'de kesiyor, F411 500'e izin veriyor** |
| Duty max | `DUTY_MAX = 4000` (`terminal_parser.c:9`) | `PWM_MAX_DUTY = 4000` | ✅ Uyumlu |
| Boost MS | `vals[i] < 0 \|\| vals[i] > 10000` (`terminal_parser.c:153`) | `> 1000` (`f411/command_handlers_config.c:116`) | **⚠️ H7 10000'e izin veriyor, F411 1000'de kesiyor** |
| Kick ms | `vals[i] < 0 \|\| vals[i] > 10000` (`terminal_parser.c:229`) | `KICK_MS_MAX = 1000` | **⚠️ H7 10000, F411 1000** |
| Kick duty | `v < 0 \|\| v > 4000` (`terminal_parser.c:179`) | `KICK_DUTY_MAX = 4000` | ✅ Uyumlu |
| PI KP/KI | `IsNumeric()` ile float parse (`terminal_parser.c:305-316`), range kontrolü **yok** | `SpeedPI_SetKp/SetKi` 0..10 clamp | **⚠️ H7 hiç range kontrolü yapmıyor** |
| Ramp UP/DOWN | `IsNumeric()` float parse, range **yok** | `SpeedPI_SetRamp` 1..10000 clamp | **⚠️ H7 hiç range kontrolü yapmıyor** |
| Telper MS | `v < 1 \|\| v > 60000` (`terminal_parser.c:340`) | `Telemetry_SetIntervalMs` 20..5000 clamp | **⚠️ H7 1..60000, F411 20..5000** |
| Base PWM bands | 0..4000 clamp var (`terminal_parser.c:121-122`) | 0..4000 (parse_u16_values) | ✅ Uyumlu |

**Kritik uyumsuzluklar:**
1. **RPM_MAX:** H7 200'de kesiyor, F411 500'e izin veriyor. Bu güvenli tarafta kalmak ama kullanıcı `rpm 300` yazarsa H7 parse etmez (200'e clamp yapar). F411'e `rpm 200` gider.
2. **Boost MS:** H7 10000'e izin veriyor, F411 1000'de kesiyor. Hata yok ama kullanıcı yanıltıcı.
3. **Kick MS:** Aynı sorun — H7 10000, F411 1000.
4. **PI ve Ramp:** H7'de range kontrolü yok. F411 clamp'lese de, H7 kullanıcıya hata mesajı göstermeli.
5. **Telper:** H7 1ms'ye izin veriyor, F411 20ms altını reddeder.

**Tavsiye:** `terminal_parser.c`'deki limitler F411 ile uyumlu hale getirilmeli. Her tuning komutu için F411 limitini yansıtan sabitler tanımlanmalı.

### Risk 6: ACK Altyapısı Yarım Kalmış

`command_handler.c:120-123` yorumunda belirtildiği gibi:
```
ACK confirmation from the F411 is NOT used: the existing ACK/OK parsing is
not wired to raw commands (SendRaw does not register a pending ACK and the
RX callback only logs replies)
```

Yani `ack_manager.c` var ama kullanılmıyor. RX tarafında `[OK]` / `[ERR]` prefix'leri parse edilmiyor. Bu, H7'nin F411'den gelen hata mesajlarını görmemesi anlamına gelir.

**Risk:** F411 `[ERR] Stop motor first` döndüğünde H7 bunu fark etmez. Kullanıcıya hata gösterilmez.

**Çözüm:** `motor_uart_dma.c` RX işleme zincirine `[OK]`, `[ERR]`, `[INFO]` prefix ayrıştırması eklenmeli. Düşük öncelikli — önce line assembler, sonra ACK parsing.

### Risk 7: GUI F446 Test Panelini 4 Motorlu Desteklemiyor

`earendil.py` zaten 4 motorlu telemetry tablosuna sahip ama F446'daki şu özellikler eksik:
- Service unlock/lock butonları
- Motor seçicili identify (şu an klavyeden I tuşu global identify)
- E-Stop butonu
- Safe/Coast stop butonu

**Risk:** Düşük — GUI sonradan güncellenebilir, firmware öncelikli.

### Risk 8: Build/Makefile Mutlak Path Sorunları

`Debug/makefile` içinde:
```makefile
# Paths
PROJECTDIR = /home/garth/H7-DMA
```
Bu mutlak path, farklı bir makinede build alırken sorun çıkarır. Ayrıca `.ioc` dosyası da CubeMX ile yeniden generate edilmemiş olabilir.

**Risk:** Build alınamayabilir. `platformio.ini` olmadığı için sadece makefile ile build alınabilir.

**Çözüm:** Projeye `platformio.ini` eklenebilir veya makefile'daki path'ler göreceli yapılmalı.

---

## 7. H7'ye Eklenecek Komut Modeli

### Motor Hedefleme Sistemi

**Mevcut (H7):**
- `f100`, `b50` → tüm motorlara
- `FL status`, `FR f100` → tek motora raw
- `ALL base 40 40 ...` → tuning

**Önerilen (genişletilmiş):**
```
FL <cmd>       → sadece Front Left motor
FR <cmd>       → sadece Front Right motor
RL <cmd>       → sadece Rear Left motor
RR <cmd>       → sadece Rear Right motor
ALL <cmd>      → 4 motorun tümüne
```

### Tüm Komut Tipleri

| Komut | Örnek | Açıklama | H7'de var mı? |
|-------|-------|----------|:-----------:|
| **status** | `FL status` | Motor durumu sorgula | ✅ |
| **hall** | `FR hall` | Hall sensör durumu | ✅ (raw ile) |
| **mode duty** | `RL mode duty` | Duty moduna geç | ✅ |
| **mode speed** | `RR mode speed` | RPM moduna geç | ✅ |
| **rpm** | `FL rpm 50` | Hedef RPM belirle | ✅ |
| **f/b** | `FR f100` | Forward/backward duty | ✅ |
| **stop** | `ALL stop` | Normal stop (rpm 0 → 15ms → stop) | ⬜ (eklenecek) |
| **safe** | `FL safe` | Coast stop | ⬜ (eklenecek) |
| **brake** | `ALL brake` | Aktif brake (x) | ✅ |
| **estop** | `ALL estop` | Acil durum | ⬜ (eklenecek) |
| **identify** | `FL identify` | Hall haritası çıkar | ⬜ (per-motor yapılacak) |
| **base** | `ALL base 40 40 45 45 50 50 55 55` | 8-band base PWM | ✅ |
| **boost** | `FL boost 40 40 45 45 50 50 55 55 150` | Boost PWM + MS | ✅ |
| **pi** | `FL pi 0.5 0.01` | PI katsayıları | ✅ |
| **ramp** | `ALL ramp 500 200` | RPM ramp hızı | ✅ |
| **kickduty** | `FR kickduty 500` | Kick puls duty | ✅ |
| **kickms** | `RL kickms 200` | Kick puls süre | ✅ |
| **telper** | `ALL telper 100` | Telemetry periyodu | ✅ |
| **clrerr** | `RR clrerr` | Hataları temizle | ⬜ (raw ile) |
| **service unlock** | `service unlock CURRENT_LIMITED_BENCH_SUPPLY` | 30sn servis kilidi aç | ⬜ (eklenecek) |
| **service lock** | `service lock` | Servis kilidini kapat | ⬜ (eklenecek) |
| **scan** | `FL scan` | Hall sinyallerini izle | ⬜ (raw ile, unlock gerek) |
| **test** | `FR test` | 6-sektör test | ⬜ (raw ile, unlock gerek) |
| **gatetest** | `RL gatetest 0 500` | Tek sektör gate test | ⬜ (raw ile, unlock gerek) |
| **help** | `help` | Komut listesi | ✅ |

---

## 8. Service Unlock Tasarımı

### Önerilen API

```
service unlock CURRENT_LIMITED_BENCH_SUPPLY    # 30sn aç
service lock                                    # hemen kapat
service status                                  # durum sorgula
```

### Hangi Komutlar Service Unlock Gerektirir?

| Komut | Gerekçe |
|-------|---------|
| `identify` | Motoru döndürebilir, hall map'i değiştirir |
| `scan` | Motor hareketiyle ilgili debug |
| `test` | 6 sektör sürer, motor döner |
| `gatetest` | MOSFET'leri döndürür, donanım riski |
| `save`, `savecfg`, `saveall` | Flash'a yazma (disabled ama yine de) |
| `map set`, `map apply`, `map reset`, `map edit`, `map discard` | Map değiştirir |
| `base`, `boost`, `pi`, `kickduty`, `kickms`, `ramp` komutları (FL/FR/RL/RR/ALL ile) | Tuning parametreleri, yanlış değer motoru yakabilir |

### Service Lock + DISARM Etkileşimi

| DISARM | Service Lock | Sonuç |
|:------:|:------------:|-------|
| Kapalı | Kapalı | Sadece güvenli komutlar (status, help, stop, brake) + passthrough |
| Kapalı | Açık | Her şey serbest (30sn) |
| Açık | (fark etmez) | Sadece mode değişimi, status, help, stop, brake — DISARM öncelikli |

### Token Doğrulama

Token F446 ile aynı: `CURRENT_LIMITED_BENCH_SUPPLY`. Case-insensitive karşılaştırma.

```c
bool ServiceLock_TryUnlock(const char *token)
{
    // F411 firmware'i lowercase yapıyor, biz de öyle yapalım
    char lower[64];
    // tolower kopyala
    if (strcmp(lower, "current_limited_bench_supply") == 0) {
        s_unlocked = true;
        s_unlockTick = HAL_GetTick();
        return true;
    }
    return false;
}
```

### Otomatik Kilitleme

- 30sn timeout → `ServiceLock_Update()` main loop'da çağrılır
- `estop` → otomatik kilitle
- Herhangi bir fault → otomatik kilitle

---

## 9. Identify Import Planı

### Per-Motor Identify Akışı

```
Kullanıcı: service unlock CURRENT_LIMITED_BENCH_SUPPLY
H7: [OK] Service unlocked for 30s

Kullanıcı: FL identify
H7:
  1. ServiceLock_CheckCommand("identify", requireUnlock=true) → geçti
  2. MotorDispatcher_SendRawToMotor(MOTOR_FL, "arm service CURRENT_LIMITED_BENCH_SUPPLY")
  3. WaitForTxDrain(100ms)  ← FL TX kanalı boşalana kadar bekle
  4. MotorDispatcher_SendRawToMotor(MOTOR_FL, "identify")
  5. Logger: [IDENTIFY][FL] Identify command dispatched
```

### ALL Identify

```
Kullanıcı: ALL identify
H7:
  1. Her motor için sırayla: arm service → TX drain → identify
  2. Veya tümüne arm service → TX drain → tümüne identify
```

**Tavsiye:** Sıralı yaklaşım daha güvenli. Her motor için:
```
arm service FL → drain → identify FL
arm service FR → drain → identify FR
arm service RL → drain → identify RL
arm service RR → drain → identify RR
```

Bu sıralı yaklaşım, F411'lerin 30sn arming timeout'unu aşmamak için her motor identify'si arasında gecikme olmamalı (4 motor × ~10ms = 40ms total, 30sn'nin çok altında).

---

## 10. Stop / Safe / Brake / E-Stop Planı

### Önerilen Servis Fonksiyonları

```c
// MotorService.h — yeni modül veya motion_controller'a ek

typedef enum {
    STOP_TARGET_FL  = (1 << 0),
    STOP_TARGET_FR  = (1 << 1),
    STOP_TARGET_RL  = (1 << 2),
    STOP_TARGET_RR  = (1 << 3),
    STOP_TARGET_ALL = 0x0F
} StopTarget_t;

void MotorService_NormalStop(StopTarget_t target);
    // target motorlara "rpm 0" gönder
    // 15ms sonra (HAL_GetTick callback veya main loop) "stop"

void MotorService_CoastStop(StopTarget_t target);
    // target motorlara "safe" gönder
    // 15ms sonra "stop"

void MotorService_BrakeStop(StopTarget_t target);
    // target motorlara "x" gönder
    // (BRAKE_HOLD_MS F411'de otomatik, H7 ekstra stop göndermez)

void MotorService_EStop(StopTarget_t target);
    // CancelPending tüm kanallar
    // target motorlara "estop" gönder
    // service lock'u kapat
    // DISARM'a geç
```

### Overwrite Riskini Çözme

**Mevcut durum:** `motor_tx_dma.c`'de her kanalın 1 active + 1 pending slot'u var. Safety komutlar (stop/x) pending'i overwrite edebilir, normal komutlar safety'i overwrite edemez.

**Risk senaryosu:** `rpm 0` gönder → TX başladı → `stop` gönder → pending slot'a yaz → TX tamamlanınca pending `stop` gider.

Bu aslında **doğru davranış**: önce `rpm 0` (rampa başlat), hemen ardından `stop` (coast). Ama rampa'nın etki etmesi için yeterli süre geçmemiş olabilir.

**Çözüm:** İki aşamalı stop'ta 15ms gecikme kullanılır. Bu süre:
- F411'de rampa'nın çalışması için yeterli (rpm 0 hedef → rampa ile yavaşlama başlar)
- DMA TX'in tamamlanması için fazlasıyla yeterli (64 byte @115200 ≈ 5.5ms)

**Tavsiye:** Mevcut tek pending slot yapısını koru. Safety sequence'lerde `WaitForTxDrain()` kullan. Çoklu safety queue ekleme (şimdilik) gereksiz.

---

## 11. Telemetry RX Planı

### Mevcut RX Problemi

`motor_uart_dma.c`'deki `HAL_UARTEx_RxEventCallback()` (262-306):
- DMA buffer dolduğunda veya Idle line'da çağrılır
- Gelen tüm veriyi (`Size` byte) `rxSlot[idx].msg`'e kopyalar
- **Satır bazlı ayırma yok**

### Line Assembler Tasarımı

```c
// motor_uart_dma.c'ye eklenecek

#define RX_LINE_MAX  128
#define RX_LINE_COUNT 8  // ring buffer boyutu

typedef struct {
    char     line[RX_LINE_MAX];
    uint16_t len;
    volatile bool ready;
} RxLineSlot_t;

typedef struct {
    // ... mevcut DMA buffer ...
    char     partial[RX_LINE_MAX];  // tamamlanmamış satır
    uint16_t partialLen;
    RxLineSlot_t lines[RX_LINE_COUNT];
    uint8_t  writeIdx;
    uint8_t  readIdx;
} MotorUartRxState_t;
```

**Line assembler akışı:**

```
DMA RX → HAL_UARTEx_RxEventCallback()
  → yeni DMA buffer'daki veriyi oku
  → her byte için:
       if (byte == '\n') {
           partial'ı lines[writeIdx]'e kopyala
           lines[writeIdx].ready = true
           writeIdx++
           partialLen = 0
       } else if (byte != '\r') {
           partial[partialLen++] = byte
           if (partialLen >= RX_LINE_MAX-1) {
               // overflow: satırı olduğu gibi kes
               partial[partialLen] = '\0'
               lines[writeIdx]'e kopyala
               partialLen = 0
           }
       }
  → DMA RX'i yeniden başlat

MotorUartDma_Update()'de:
  → lines[readIdx].ready olanları işle
  → [OK], [ERR], [INFO], telemetry (RPM:) ayrımı yap
  → ReadIdx ilerlet
```

### Telemetry Sınıflandırma

Gelen satırlar 4 kategoriye ayrılmalı:

| Kategori | Prefix | Örnek | İşlenmesi |
|----------|--------|-------|-----------|
| **Telemetry** | `RPM:` | `RPM:23,T:30,...` | `[TEL][FL]` ile log'la, GUI'ye gönder |
| **OK** | `[OK]` | `[OK] Service armed for 30s` | `[ACK][FL]` ile log'la |
| **ERR** | `[ERR]` | `[ERR] Stop motor first` | `[ERR][FL]` ile log'la, kullanıcıya göster |
| **INFO** | `[INFO]` | `[INFO] Mode=SPEED` | `[INFO][FL]` ile log'la |

### GUI Telemetry Görüntüleme

Mevcut `earendil.py` formatı zaten doğru:
```
[TEL][FL] RPM:23,T:30,D:67,...
```

GUI regex: `r"(?:\[INFO\]\s*)?\[TEL\]\[(FL|FR|RL|RR)\]\s+(RPM:.*)$"`

**Stale/ missing telemetry tespiti:**
- Her motor için son telemetry zamanı kaydedilmeli
- 2sn içinde telemetry gelmezse "LOST" veya "NO DATA" gösterilmeli
- GUI'de `SafetyManager_IsLinkLost()` durumu da gösterilebilir (şu an sadece log'da)

---

## 12. Parser Limit Uyumluluğu Düzeltme Planı

### `terminal_parser.c`'de Değişmesi Gereken Limitler

| Sabit | Mevcut (H7) | Olması Gereken (F411) | Dosya/Satır |
|-------|:-----------:|:---------------------:|-------------|
| `RPM_MAX` | 200 | 500 | `terminal_parser.c:8` |
| `DUTY_MAX` | 4000 | 4000 | ✅ zaten uyumlu |
| Boost MS kontrolü | `> 10000` | `> 1000` | `terminal_parser.c:153` |
| Kick ms kontrolü | `> 10000` | `> 1000` | `terminal_parser.c:229` |
| PI KP/KI float parse | range kontrolü yok | 0..10 | `terminal_parser.c:305-316` |
| Ramp float | range kontrolü yok | 1..10000 | `terminal_parser.c:269-290` |
| Telper MS | `1..60000` | `20..5000` | `terminal_parser.c:340` |

### Değişiklik Planı

```c
// terminal_parser.c — üst kısma eklenecek
// F411 motor firmware limitlerini yansıtır
// (f411-motor-cube/App/inc/app_config.h ile senkronize)
#define F411_RPM_MAX        500u
#define F411_DUTY_MAX       4000u
#define F411_BOOST_MS_MAX   1000u
#define F411_KICK_MS_MAX    1000u
#define F411_PI_GAIN_MAX    10.0f
#define F411_RAMP_MIN       1.0f
#define F411_RAMP_MAX       10000.0f
#define F411_TELPER_MIN     20u
#define F411_TELPER_MAX     5000u
```

---

## 13. GUI Import Planı

### Mevcut GUI (`earendil.py`) Durumu

**Var olan:**
- 4 motor telemetry tablosu (`_motor_telemetry` dict, `_update_motor_telemetry()`)
- Motor tuning dialog (`MotorSettingsDialog`)
- Klavye kısayolları (WASD, Space, X, M, I)
- Operating mode indicator
- Serial port bağlantı yönetimi
- UART error log parsing

**Eksik olan:**
- Service Unlock / Lock butonları
- Motor seçicili Identify butonu
- E-Stop, Safe Stop butonları
- Blocked command counter display
- Identify onay diyalogu

### Önerilen GUI Yapısı

```
┌──────────────────────────────────────────────────────────────┐
│ [FL] [FR] [RL] [RR] [ALL]     ← Motor seçici tabs/buttons   │
├──────────────────────────────────────────────────────────────┤
│ ┌─ Motor Commands ───────────────────────────────────────┐  │
│ │ [Status] [Hall] [Mode Duty] [Mode Speed]               │  │
│ │ [Forward Duty] ── [+] [-] [Backward Duty] ── [+] [-]  │  │
│ │ [RPM Test] ── [+] [-]                                  │  │
│ │ [Normal Stop] [Safe Stop] [Brake] [E-Stop]             │  │
│ └────────────────────────────────────────────────────────┘  │
│ ┌─ Service ───────────────────────────────────────────────┐  │
│ │ [Service Unlock] Token: [_______________] [Apply]       │  │
│ │ [Service Lock]  Status: [LOCKED / UNLOCKED (XXs)]      │  │
│ │ Blocked commands: [0]                                   │  │
│ └────────────────────────────────────────────────────────┘  │
│ ┌─ Diagnostics ───────────────────────────────────────────┐  │
│ │ [Identify] [Scan] [Test] [Gate Test]                    │  │
│ │ (Onay: "Motor unloaded? PSU limited? E-stop accessible?)│  │
│ └────────────────────────────────────────────────────────┘  │
│ ┌─ Telemetry ─────────────────────────────────────────────┐  │
│ │ FL: RPM:23  T:30  D:67  DIR:F  H:5  ...                │  │
│ │ FR: RPM:0   T:0   D:0   DIR:N  H:0  ...                │  │
│ │ RL: RPM:0   T:0   D:0   DIR:N  H:0  ...                │  │
│ │ RR: RPM:0   T:0   D:0   DIR:N  H:0  ...                │  │
│ └────────────────────────────────────────────────────────┘  │
│ ┌─ Console ───────────────────────────────────────────────┐  │
│ │ [TEL][FL] RPM:23,T:30,D:67,...                          │  │
│ │ [INFO] [MODE] MANUAL active                              │  │
│ │ CMD: FL f100                                             │  │
│ └────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
```

### Identify Onay Diyaloğu

```python
def _confirm_identify(self, motor_tag: str) -> bool:
    msg = QMessageBox(self)
    msg.setIcon(QMessageBox.Warning)
    msg.setWindowTitle("Confirm Identify")
    msg.setText(f"Run Identify on {motor_tag}?")
    msg.setInformativeText(
        "• Motor must be UNLOADED (no propeller/wheel)\n"
        "• Current-limited PSU connected\n"
        "• E-stop accessible\n"
        "• Motor will rotate during identification"
    )
    msg.setStandardButtons(QMessageBox.Ok | QMessageBox.Cancel)
    return msg.exec() == QMessageBox.Ok
```

---

## 14. Dosya Bazlı Değişiklik Planı

### `h7-main/Core/Inc/rover_types.h`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | Mevcut enum'lar: MotorId_t, Direction_t, MotorDir_t, MotorCmd_t, MotionCmd_t, AckStatus_t, LinkState_t, RoverMode_t |
| **Değişecek** | Yok (mevcut yapı yeterli) |
| **Eklenecek** | `StopType_t` enum (NORMAL, COAST, BRAKE, ESTOP) |
| **Korunacak** | MotorId_t sırası (FL=0, FR=1, RL=2, RR=3) — tüm sistem buna bağlı |
| **Risk** | Enum sırası değişirse tüm mapping bozulur |

### `h7-main/Core/Inc/app_config.h`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | UART handle mapping, DMA handle mapping, safety timeout'lar |
| **Değişecek** | Yok (mevcut yapı yeterli) |
| **Eklenecek** | Varsa F411 limit sabitleri (opsiyonel — terminal_parser.c'de de tanımlanabilir) |
| **Korunacak** | MOTOR_UART_HANDLE(), MOTOR_DMA_RX_HANDLE() macro'ları |

### `h7-main/Core/Src/terminal_parser.c` + `.h`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | Tüm parse mantığı, limitler, tuning validasyonu |
| **Değişecek** | `RPM_MAX` → 500, Boost MS limit → 1000, Kick ms limit → 1000, Telper limit → 20..5000, PI/Ramp range validasyonu eklenecek |
| **Eklenecek** | `TCMD_ESTOP`, `TCMD_SAFE`, `TCMD_SERVICE` enum değerleri + parse dalları |
| **Korunacak** | Mevcut tüm komut parse mantığı (f/b/r/l/fd/bd/rd/ld, tuning, raw) |
| **Risk** | Limit değişiklikleri mevcut davranışı değiştirir — test edilmeli |

### `h7-main/Core/Src/command_handler.c` + `.h`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | DISARM gate, identify akışı, mode switch, help, raw motor dispatch |
| **Değişecek** | DISARM gate'e `TCMD_ESTOP`, `TCMD_SAFE`, `TCMD_SERVICE` eklenecek. `TCMD_STOP` → `MotionController_StopNormal()` çağıracak. `identify` servis kilidi kontrolü ekleyecek. |
| **Eklenecek** | `IsDangerousRawPayload()` (F446'daki `isDangerousServiceCmd`'in H7 versiyonu). `TCMD_ESTOP`, `TCMD_SAFE`, `TCMD_SERVICE` handler'ları. |
| **Korunacak** | Mevcut handler yapısı, `IsSafeRawPayload()`, mode switch, tuning dispatch |
| **Risk** | `identify`'ın DISARM allowlist'ten çıkarılması mevcut kullanıcıları etkileyebilir |

### `h7-main/Core/Src/motor_dispatcher.c` + `.h`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | Send, SendAll, SendRaw, SendRawToMotor, SendTunePayload |
| **Değişecek** | Yok (mevcut API yeterli) |
| **Eklenecek** | `MotorDispatcher_SendRawToTarget(StopTarget_t target, const char *msg)` — birden fazla motor hedefleme maskesi |
| **Korunacak** | Defense-in-depth DISARM gate, log formatı, MotorLink güncelleme |
| **Risk** | Yeni fonksiyon eklenirse tüm dispatcher kullanıcıları güncellenmeli |

### `h7-main/Core/Src/motor_tx_dma.c` + `.h`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | Active + pending slot mantığı, safety prioritization, CancelPending, AllIdle |
| **Değişecek** | **Yok** — mevcut yapı safety sequence'ler için yeterli |
| **Eklenecek** | Yok |
| **Korunacak** | `IsSafetyCommand()`, pending overwrite kuralları, DMA buffer yapısı |
| **Risk** | Eğer çoklu safety queue eklenirse bu dosya kökten değişir — şimdilik gerekli değil |

### `h7-main/Core/Src/motor_uart_dma.c` + `.h`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | DMA RX callback, error recovery, slot yapısı, telemetry log formatı |
| **Değişecek** | **RX line assembler eklenecek** — mevcut `rxSlot` yapısı line bazlı çalışacak şekilde genişletilecek |
| **Eklenecek** | `LineAssembler_PutChar()`, `LineAssembler_GetLine()`, `[OK]`/`[ERR]`/`[INFO]` prefix ayrıştırma |
| **Korunacak** | DMA buffer yapısı, error recovery, SafetyManager_NotifyRx çağrıları |
| **Risk** | Line assembler ISR context'te çalışacağı için dikkatli implementasyon gerek — heap kullanımı yasak, printf yasak, döngüler kısa |

### `h7-main/Core/Src/safety_manager.c` + `.h`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | Link-loss tespiti, DISARM enter/leave, recovery log |
| **Değişecek** | Yok (mevcut yapı yeterli) |
| **Eklenecek** | `SafetyManager_GetBlockedCount()` — servis kilidi tarafından bloke edilen komut sayısı (opsiyonel, service_lock modülünde de olabilir) |
| **Korunacak** | Link-loss timeout (3s), volatile değişkenler, ISR-safe tasarım |
| **Risk** | Düşük |

### `h7-main/Core/Src/motion_controller.c` + `.h`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | Execute, Stop, DisarmSafe fonksiyonları |
| **Değişecek** | `MotionController_Stop()` → `MotionController_StopNormal()` (iki aşamalı). Mevcut `Stop()` davranışı değişecek. |
| **Eklenecek** | `MotionController_StopNormal()`, `MotionController_StopCoast()`, `MotionController_StopEstop()`, `MotionController_ServiceDelayedStop()` (main loop) |
| **Korunacak** | Tank-drive steering mantığı, Execute fonksiyonu |
| **Risk** | `Stop()`'un davranışı değişirse DISARM enter/exit akışı etkilenebilir. `SafetyManager_EnterDisarm()` içinde `MotionController_Stop()` çağrılıyor — bu da iki aşamalı olursa DISARM gecikebilir. Çözüm: DISARM context'inde direkt stop (iki aşamalı değil). |

### `h7-main/earendil.py`

| İşlem | Detay |
|-------|-------|
| **İncelenecek** | Mevcut GUI widget'ları, telemetry parsing, command sending |
| **Değişecek** | Service unlock/lock UI eklenecek. Identify onay diyaloğu eklenecek. E-Stop/Safe Stop butonları eklenecek. |
| **Eklenecek** | Motor seçici (FL/FR/RL/RR/ALL) tab/button grubu. Identify onay dialog'u. Blocked command counter. |
| **Korunacak** | Mevcut telemetry tablosu, tuning dialog, klavye kısayolları, theme sistemi |
| **Risk** | Düşük — GUI ayrı bir süreç, firmware değişikliklerinden bağımsız |

---

## 15. Uygulama Sırası

### Aşama 0: Build Altyapısı

```bash
# Mutlak path sorununu çöz
# platformio.ini ekle (veya makefile path'lerini relative yap)
# Build'i dene
make -C h7-main/Debug clean && make -C h7-main/Debug
# veya
pio run -d h7-main  # eğer platformio.ini eklenirse
```

**Kontrol:** `H7-DMA.elf` üretilmeli, hata olmamalı.

### Aşama 1: Parser Limit Düzeltmeleri

**Dosyalar:** `terminal_parser.c`

1. `RPM_MAX` 200 → 500
2. Boost MS limit 10000 → 1000
3. Kick ms limit 10000 → 1000
4. Telper limit 1..60000 → 20..5000
5. PI KP/KI range kontrolü (0..10)
6. Ramp range kontrolü (1..10000)

**Test:** `python3 -c "from terminal_parser import *"` — yok, build test.
**Kontrol:** Build geçmeli, mevcut test komutları (f100, b50, rpm 30) aynı çalışmalı.

### Aşama 2: Service Lock Modülü

**Dosyalar:** `service_lock.c` (yeni), `service_lock.h` (yeni), `terminal_parser.c`, `command_handler.c`

1. `service_lock.c/h` oluştur — F446'daki `serviceUnlocked` + timeout
2. `terminal_parser.c`'ye `service unlock <token>`, `service lock`, `service status` komutlarını ekle
3. `command_handler.c`'ye handler'ları ekle
4. `IsDangerousRawPayload()` fonksiyonunu ekle (F446'daki liste)
5. `command_handler.c`'de `TCMD_MOTOR_RAW` handler'ına servis kilidi kontrolü ekle

**Test:** `service unlock YANLIS_TOKEN` → ERR. `service unlock CURRENT_LIMITED_BENCH_SUPPLY` → OK. 30sn sonra otomatik kilit.
**Kontrol:** Build geçmeli, service unlock olmadan `FL identify` bloklanmalı.

### Aşama 3: RX Line Assembler

**Dosyalar:** `motor_uart_dma.c`, `motor_uart_dma.h`

1. `MotorUartRxState_t` yapısını ekle (partial buffer + line ring)
2. `LineAssembler_Feed()` fonksiyonunu ekle (ISR-safe)
3. `HAL_UARTEx_RxEventCallback()`'i güncelle — DMA verisini line assembler'dan geçir
4. `MotorUartDma_Update()`'i güncelle — tam satırları işle
5. `[OK]`, `[ERR]`, `[INFO]`, telemetry prefix ayrımını ekle

**Test:** F411'den gelen çok satırlı telemetry'nin doğru bölündüğünü doğrula.
**Kontrol:** Build geçmeli, `[TEL][FL] RPM:...` formatı korunmalı.

### Aşama 4: İki Aşamalı Stop + E-Stop

**Dosyalar:** `motion_controller.c`, `motion_controller.h`, `command_handler.c`

1. `MotionController_StopNormal()` — `rpm 0` + gecikmeli `stop`
2. `MotionController_StopCoast()` — `safe` + gecikmeli `stop`
3. `MotionController_StopEstop()` — direkt `estop` + service lock
4. `MotionController_ServiceDelayedStop()` — main loop'da çağrılacak
5. `TCMD_STOP` handler → `MotionController_StopNormal()`
6. `TCMD_SAFE` handler → `MotionController_StopCoast()`
7. `TCMD_ESTOP` handler → `MotionController_StopEstop()`
8. `app_main.c` — `App_Update()`'e `MotionController_ServiceDelayedStop()` çağrısı ekle

**Test:** `stop` → log'da "rpm 0" ardından 15ms sonra "stop" görülmeli.
**Kontrol:** DISARM enter/exit akışı bozulmamalı — `SafetyManager_EnterDisarm()` direkt stop kullanmalı, iki aşamalı değil.

### Aşama 5: Per-Motor Identify

**Dosyalar:** `command_handler.c`, `terminal_parser.c`

1. `TCMD_IDENTIFY`'yi per-motor yap: `FL identify`, `FR identify`, `ALL identify`
2. Her identify öncesi servis kilidi kontrolü
3. `arm service + TX drain + identify` sırası per-motor
4. `ALL identify` = 4 motor için sıralı identify

**Test:** `FL identify` → FL motoruna `arm service` + `identify`. `ALL identify` → 4 motor.
**Kontrol:** Service unlock gerekli. `FR identify` sadece FR motoruna gitmeli.

### Aşama 6: GUI Güncellemesi

**Dosyalar:** `earendil.py`

1. Motor seçici tab/button grubu (FL/FR/RL/RR/ALL)
2. Service Unlock (token input) + Service Lock butonu
3. Identify butonu + onay diyaloğu
4. E-Stop butonu
5. Safe Stop butonu
6. Blocked command counter display

**Test:** GUI'den FL identify → onay → serial'de `FL identify`. Service unlock → identify çalışsın.
**Kontrol:** Mevcut klavye kısayolları (WASD, Space, X) çalışmaya devam etmeli.

### Aşama 7: Entegrasyon Testleri

**Testler:** Aşağıdaki bölüm 16'da detaylandırılmıştır.

---

## 16. Test Planı

### Aşama 1: Motorsuz UART Testleri

| Test | Beklenen |
|------|---------|
| H7 boot → terminal'de `[BOOT] H723 rover main controller started` | ✅ |
| `help` → komut listesi | ✅ |
| `status` → 4 motora da `status` gider, F411 yoksa link-loss log'lanır | ✅ |
| `mode disarm` → DISARM aktif, kırmızı LED | ✅ |
| `mode manual` → MANUAL aktif, yeşil LED | ✅ |
| `service unlock CURRENT_LIMITED_BENCH_SUPPLY` → OK | ✅ |
| `service lock` → OK | ✅ |
| `FL status` → FL status gider | ✅ |
| `ALL status` → 4 motor status | ✅ |

### Aşama 2: Tek F411 Sıralı Port Testi

| Test | Beklenen |
|------|---------|
| F411'i FL portuna (USART2) tak → `FL status` → telemetry döner | ✅ |
| F411'i FR portuna (UART4) tak → `FR status` → telemetry döner | ✅ |
| F411'i RL portuna (UART7) tak → `RL status` → telemetry döner | ✅ |
| F411'i RR portuna (UART5) tak → `RR status` → telemetry döner | ✅ |

### Aşama 3: 4 F411 Telemetry Testi

| Test | Beklenen |
|------|---------|
| 4 F411 bağlı → `ALL status` → 4 telemetry de gelir | ✅ |
| `ALL telper 100` → 4 motor da 100ms'de bir telemetry gönderir | ✅ |
| GUI'de FL/FR/RL/RR telemetry tablosu güncellenir | ✅ |
| Bir F411 kablosunu çek → link-loss log'u (3sn) | ✅ |

### Aşama 4: Mode Duty/Speed Testi

| Test | Beklenen |
|------|---------|
| `mode duty` → F411'ler duty moduna geçer | ✅ |
| `mode speed` → F411'ler speed moduna geçer | ✅ |
| Geçiş öncesi `stop` gönderilir (F411 "Stop motor first" hatası almaz) | ✅ |
| `m speed` alias da çalışır | ✅ |

### Aşama 5: Düşük PWM Hareket Testi

| Test | Beklenen |
|------|---------|
| `FL f50` → FL motoru düşük duty'de döner | ✅ |
| `FR b30` → FR motoru geri döner | ✅ |
| `ALL b100` → 4 motor da geri döner | ✅ |
| `ALL stop` → 4 motor da `rpm 0` + 15ms + `stop` | ✅ |

### Aşama 6: Düşük RPM Hareket Testi

| Test | Beklenen |
|------|---------|
| `mode speed` → RPM modu | ✅ |
| `FL rpm 30` → FL motoru 30 RPM'de döner | ✅ |
| `FR rpm -20` → FR motoru 20 RPM geri | ✅ |
| `ALL rpm 0` → 4 motor da durur | ✅ |

### Aşama 7: Per-Motor Identify Testi

| Test | Beklenen |
|------|---------|
| Service unlock olmadan `FL identify` → bloke | ✅ |
| `service unlock ...` → OK | ✅ |
| `FL identify` → FL motor identify başlatır | ✅ |
| Sadece FL motoru identify olur, diğerleri etkilenmez | ✅ |

### Aşama 8: ALL Identify Testi

| Test | Beklenen |
|------|---------|
| `ALL identify` → 4 motor sırayla identify | ✅ |
| Her identify arasında TX drain beklenir | ✅ |
| 4 identify de tamamlanır (30sn service timeout içinde) | ✅ |

### Aşama 9: Stop / Safe / Brake / E-Stop Testi

| Test | Beklenen |
|------|---------|
| Motor dönerken `ALL stop` → önce rpm 0, 15ms sonra stop | ✅ |
| Motor dönerken `ALL safe` → önce safe, 15ms sonra stop | ✅ |
| Motor dönerken `ALL brake` → anında x (aktif brake) | ✅ |
| Motor dönerken `ALL estop` → anında estop, service lock kapanır, DISARM'a geçer | ✅ |
| Estop sonrası `FL f50` → bloke (DISARM) | ✅ |
| `mode manual` + tekrar `FL f50` → çalışır | ✅ |

### Aşama 10: Overwrite / Sıra Testi

| Test | Beklenen |
|------|---------|
| Hızlı ardışık `f100` → `stop` → `f200` → `stop` → son komut stop olmalı | ✅ |
| `brake` (x) → hemen `stop` → pending: stop (safety overwrite) | ✅ |
| `f100` (TX başladı) → `f200` → pending: f200 (normal overwrite) | ✅ |
| `f100` (TX başladı) → `x` → pending: x (safety overwrite) | ✅ |

---

## 17. Kabul Kriterleri

| # | Kriter | Nasıl Doğrulanır? |
|---|--------|-------------------|
| 1 | H7 her motoru doğru UART üzerinden sürebiliyor | FL→USART2, FR→UART4, RL→UART7, RR→UART5 bağlı F411'lerden telemetry geliyor |
| 2 | Her motor ayrı ayrı status/mode/rpm/f/b/stop/safe/brake/identify destekliyor | `FL status`, `FR status`, `RL status`, `RR status` ayrı ayrı çalışır |
| 3 | `ALL` komutları 4 motora güvenli şekilde gidiyor | `ALL stop` → 4 motor da durur, `ALL status` → 4 telemetry gelir |
| 4 | Service unlock olmadan identify/test/gatetest çalışmıyor | `FL identify` → `ERR|service locked` |
| 5 | Telemetry GUI'de motor bazlı ayrılıyor | GUI'de FL/FR/RL/RR ayrı sütunlarda gösterilir |
| 6 | Stop/brake sıraları overwrite olmuyor | Safety komutlar her zaman pending slot'u alır |
| 7 | H7 parser F411 limitleriyle uyumlu | `terminal_parser.c` limitleri F411 `app_config.h` ile eşleşir |
| 8 | GUI'den 4 motorlu test yapılabiliyor | Motor seçici + komut butonları + telemetry tablosu çalışır |
| 9 | 15ms gecikmeli stop mekanizması çalışıyor | Log'da `rpm 0` → 15ms sonra `stop` görülür |
| 10 | DISARM mode safety gate çalışıyor | DISARM'da motion komutları bloke, `mode manual` ile açılır |

---

## 18. Referans Dosya Haritası

| Dosya | Satır Sayısı | Rol |
|-------|:-----------:|-----|
| `h7-main/Core/Inc/rover_types.h` | 80 | Temel tipler, enum'lar |
| `h7-main/Core/Inc/app_config.h` | 52 | Pin/UART mapping, timeout'lar |
| `h7-main/Core/Inc/terminal_parser.h` | 89 | Komut tipleri, parse sonucu struct |
| `h7-main/Core/Src/terminal_parser.c` | 638 | Tüm terminal komut ayrıştırma |
| `h7-main/Core/Src/command_handler.c` | 422 | Komut yönlendirme, DISARM gate, identify, mode switch |
| `h7-main/Core/Src/motor_dispatcher.c` | 198 | 4 motorlu komut dağıtımı |
| `h7-main/Core/Src/motor_tx_dma.c` | 267 | DMA TX engine, pending slot, safety priority |
| `h7-main/Core/Src/motor_uart_dma.c` | 343 | DMA RX engine, error recovery |
| `h7-main/Core/Src/motion_controller.c` | 85 | Tank-drive steering, stop |
| `h7-main/Core/Src/safety_manager.c` | 126 | Link-loss, DISARM safety |
| `h7-main/Core/Src/operating_mode.c` | 46 | Rover mode state machine |
| `h7-main/Core/Src/app_main.c` | 75 | Init/Update loop |
| `h7-main/Core/Src/main.c` | 600 | CubeMX init, UART yapılandırma |
| `h7-main/earendil.py` | 2569 | Python PySide6 GUI |

---

## 19. Ek: Build Komutları

```bash
# Mevcut build (makefile)
make -C h7-main/Debug clean && make -C h7-main/Debug

# Eğer platformio.ini eklenirse
pio run -d h7-main

# Python araç kontrol
python -m py_compile tools/terminal.py
python -m py_compile h7-main/earendil.py

# GUI çalıştırma
python h7-main/earendil.py

# Terminal test (ttyUSB0 veya ST-Link VCP)
screen /dev/ttyACM0 115200
# veya
python -c "
import serial
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.1)
s.write(b'help\n')
print(s.read(1024).decode())
"
```
