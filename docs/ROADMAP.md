# ROADMAP — BLDC Motor Driver Module

> **Project:** STM32 BLDC Motor Driver (Black Pill F411CE)
> **Current State:** Working prototype with WASD Python host (legacy)
> **Target State:** 4-motor skid-steer vehicle with hub STM32
> **Design Principle:** Timer/event-based async architecture, stop≠brake separation, multi-motor readiness

---

## Tasarım Kararları Özeti

Bu roadmap'e yeni eklenen iki temel tasarım kararı:

### 1. Timer/Event Tabanlı Asenkron Mimarinin Korunması

Mevcut kodda zaten var olan yapı korunacak ve güçlendirilecek:
- PWM üretimi timer tabanlı (`runMotorControlScheduler()` 60µs tick)
- Hall değişimi event-driven / ISR tabanlı (`hallISR`)
- Kontrol/failsafe/protection ayrı bir control tick/scheduler ile yürür
- UART yalnızca komut girişi ve hedef durum güncelleme rolünde kalır
- Komutlar doğrudan MOSFET sürmez, `pendingReq` üzerinden intent düzeyinde kalır

**Neden:** Motor kontrol hot path'i ile UART parsing birbirinden ayrılmış kalmalı. Bu ayrım korunmazsa 4 motorlu yapıda timing garantisi verilemez.

### 2. Stop ≠ Brake Ayrımı

Mevcut durumda `stopMotorImmediate()` → `allOff()` = **coast stop** (tüm MOSFET'ler kapatılır, motor serbest kalır).

Hedef: Stop ve brake ayrı state/komut olarak tanımlanacak:
- **Stop (s):** Coast stop — tüm çıkışlar kapatılır, motor serbest kalır
- **Brake (k):** Aktif fren — kontrollü dinamik fren (low-side short)

**Neden:** Brake, stop'un yerine geçmemeli. Watchdog/fault durumlarında default behavior **brake değil, coast/all-off** olmalı. Brake kullanıcı kontrollü, bilinçli bir eylem olmalı.

---

## Faz Bazlı Ayrım

Bu roadmap'te her madde şu etiketlerle işaretlenir:
- `[bugfix]` — Mevcut kodda düzeltilmesi gereken bug
- `[architecture]` — Mimari düzeltme veya iyileştirme
- `[safety]` — Güvenlik / failsafe işi
- `[protocol]` — Protokol değişikliği
- `[brake]` — Brake ile ilgili
- `[multi-motor]` — 4 motorlu gelecek hazırlığı
- `[documentation]` — Dokümantasyon
- `[validation]` — Test/doğrulama

---

## Phase 0: Mevcut Durumun Doğru Belgelenmesi

**Amaç:** Mevcut kodda zaten olan yapıları ve eksik olan yapıları net biçimde ayırmak.

**Neden gerekli:** Yeni roadmap'in temeli doğru bir mevcut durum analizi. Yanlış belgeleme yanlış yönlendirir.

### Zaten Mevcut Olan Yapılar

| Yapı | Durum | Kod Referansı |
|------|-------|---------------|
| Scheduler / control tick | ✅ Mevcut | `runMotorControlScheduler()` — 60µs tick, catch-up ile |
| Hall cache / ISR temelli yapı | ✅ Mevcut | `hallISR()` — hafif, sadece pin okuma + flag |
| UART ring/queue yaklaşımı | ✅ Mevcut | `RxRing` (128B) + `CommandQueue` (8 item) |
| Software watchdog altyapısı | ✅ Mevcut | `checkCommandWatchdog()` — 800ms, Normal/Settings |
| Python mode / host kontrollü mod | ✅ Mevcut | `processPythonCommand()` — WASD handler |
| Deferred command apply | ✅ Mevcut | `pendingReq` → `applyPendingRequests()` |
| EEPROM kalıcılık | ✅ Mevcut | Hall map, config, mode — magic + checksum |
| RPM hesaplama | ✅ Mevcut | `calculateRPM()` — hall periodundan |

### Eksik veya Hedefe Uyumsuz Yapılar

| Yapı | Durum | Açıklama |
|------|-------|----------|
| UART protokolü hâlâ klavye semantiği | ❌ Eksik | w/s/x/d/a gönderiliyor, hedef f/b/s |
| Python mode'da lease/watchdog | ❌ Eksik | Watchdog kapalı, lease anlamsız |
| Default PWM hedef değer uyumsuzluğu | ❌ Eksik | 60, hedef 150 (yapılandırılabilir olmalı) |
| Stale command/backlog riski | ❌ Eksik | Kuyruk 1 komut/işlem, timestamp yok |
| Latest-command-wins / mailbox | ❌ Eksik | Kuyruk sıralı, son komut garantisi yok |
| Çok motorlu soyutlama | ❌ Eksik | Tek global state, hardcoded pinler |
| Brake state | ❌ Eksik | Sadece coast stop var |
| Donanımsal watchdog (IWDG) | ❌ Eksik | Yok |

**Yapılacak İşler:**
- [ ] Mevcut yapıları doğrula ve belgele [documentation]
- [ ] Eksik yapıları sınıflandır ve önceliklendir [documentation]
- [ ] ISSUES.md'yi güncelle [documentation]

**Başarı Kriterleri:**
- Mevcut vs eksik yapılar net biçimde ayrılmış
- Her eksik yapı bir faza atanmış

**Bu faz tamamlanmadan sonraki faza geçilebilir mi:** Evet, bu faz dokümantasyon odaklı

---

## Phase 1: Tek Motor Asenkron Timer/Event Mimarisinin Stabilize Edilmesi

**Amaç:** Mevcut timer/event tabanlı asenkron mimariyi stabilize etmek ve hedef davranışla uyumlu hale getirmek.

**Neden gerekli:** Mevcut kodda scheduler/hall/UART ayrımı zaten var ama bazı parçalar hedef mimariyle uyumsuz (kuyruk darboğazı, watchdog eksikliği, identify timing).

### Yapılacak Teknik İşler

- [x] [protocol] `processCommand()`'da f/b/s komut parsing uygula
- [x] [protocol] `processPythonCommand()`'dan w/s/x/d/a kaldır, f/b/s ekle
- [x] [protocol] Lease-tabanlı motion: her f/b/s komutu `lastMotorCommandMs` yeniler
- [x] [protocol] Python modunda watchdog'u aktifleştir (`checkCommandWatchdog()`)
- [x] [protocol] Telemetri field'larını tutarlı hale getir
- [x] [protocol] WASD-specific kodu temizle

**Etkilenen Modüller:**
- `processCommand()` — main.cpp
- `processPythonCommand()` — main.cpp
- `loop()` — main.cpp (Python branch'ta watchdog ekle)
- `sendTelemetry()` / `sendPythonTelemetry()` — main.cpp

**Riskler:**
- Protokol değişikliği mevcut Python host'u bozar (Phase 3'te güncellenecek)
- Watchdog aktifleştirmesi heartbeat timing'iyle uyumsuz olabilir

**Başarı Kriterleri:**
- `f150` → motor ileri PWM 150'de çalışır
- `b100` → motor geri PWM 100'de çalışır
- `s` → motor durur (coast)
- 800ms komut gelmezse motor otomatik durur
- Telemetri tutarlı

**Bu faz tamamlanmadan sonraki faza geçilebilir mi:** Hayır — protokol değişikliği Python host'u da etkiler

---

## Phase 3: Lease/Watchdog/Failsafe Davranışının Netleştirilmesi

**Amaç:** Lease semantiğini, watchdog davranışını ve failsafe yollarını net kurallara bağlamak.

**Neden gerekli:** Protokol değişikliği sonrası lease/watchdog davranışları tanımlanmalı. Watchdog/fault durumunda brake değil coast default olmalı.

### Lease Kuralları

| Kural | Açıklama |
|-------|----------|
| Lease yenileme | Her geçerli f/b/s komutu `lastMotorCommandMs` yeniler |
| Lease timeout | 800ms komut gelmezse motor otomatik durur |
| Lease scope | Sadece hareket komutları lease yeniler (s, status, mode yenilemez) |
| Heartbeat aralığı | Python host ~600ms'de bir komut göndermeli |

### Watchdog/Fault Default Davranışları

| Durum | Default Aksiyon | Açıklama |
|-------|-----------------|----------|
| Lease timeout | Coast stop (allOff) | Motor serbest kalır |
| Hall timeout | Fault + Coast stop | Motor serbest kalır, fault kodu set |
| Transition spam | Fault + Coast stop | Motor serbest kalır, fault kodu set |
| IWDG reset | MCU reset → allOff | Donanımsal sıfırlama |
| Host disconnect | Coast stop | UART aktivitesi yoksa dur |

**Neden brake değil coast:** Brake aktif bir eylem (akım tüketir, MOSFET sürer). Watchdog/fault durumlarında güvenli durum = tüm çıkışlar kapalı. Brake sadece kullanıcı kontrollü, bilinçli bir eylem olmalı.

**Yapılacak Teknik İşler:**
- [ ] [safety] Lease kurallarını belgele ve uygula
- [ ] [safety] Python modunda watchdog'u aktifleştir
- [ ] [safety] Fault durumlarında default behavior'ı doğrula (coast olmalı)
- [ ] [safety] Host connection monitor ekle (2sn aktivite yoksa stop)
- [ ] [documentation] Watchdog/fault behavior matrix'i belgele

**Etkilenen Modüller:**
- `checkCommandWatchdog()` — main.cpp
- `loop()` — main.cpp
- `triggerFault()` — main.cpp
- ISSUES.md

**Riskler:**
- Watchdog aktifleştirmesi beklenmedik durmalara neden olabilir (heartbeat test edilmeli)
- Host connection monitor yanlış pozitif verebilir

**Başarı Kriterleri:**
- Lease kuralları belgelenmiş ve uygulanmış
- Python modunda watchdog aktif
- Fault/watchdog durumunda motor coast durumda durur
- Host disconnect motoru durdurur

**Bu faz tamamlanmadan sonraki faza geçilebilir mi:** Hayır — safety kuralları brake eklemeden önce net olmalı

---

## Phase 4: Stop vs Brake State Machine Ayrımı [brake]

**Amaç:** Stop (coast) ve brake (aktif fren) davranışlarını ayrı state ve komut olarak tanımlamak.

**Neden gerekli:** Mevcut yapıda sadece coast stop var. Brake, kontrollü durma gerektiren senaryolar için gerekli ama stop'un yerine geçmemeli.

### Stop vs Brake Karşılaştırması

| Özellik | Stop (s) | Brake (k) |
|---------|----------|----------|
| MOSFET durumu | Tüm çıkışlar kapalı | Low-side MOSFET'ler kısa devre |
| Motor davranışı | Serbest kalır (coast) | Kontrollü yavaşlar (dynamic brake) |
| Akım | Yok | Kısa devre akımı (motor EMF'sinden) |
| Güvenli durum | Evet | Hayır (akım riski) |
| Watchdog default | Bu | Bu değil |
| Fault default | Bu | Bu değil |
| Kullanıcı komutu | `s` | `k` |

### Yeni State Machine

```
                    ┌───────────┐
                    │  Stopped  │←──── coast (s), watchdog, fault
                    └─────┬─────┘
                          │ f/b command
                    ┌─────▼─────┐
                    │   Kick    │ (optional)
                    └─────┬─────┘
                          │ kickMs timeout
                    ┌─────▼─────┐
                    │  Running  │
                    └──┬────┬──┘
                       │    │
              stop (s) │    │ brake (k)
                       │    │
              ┌────────▼┐  ┌▼────────┐
              │ Stopped  │  │ Braking │
              │ (coast)  │  │ (active)│
              └──────────┘  └────┬────┘
                                 │ brake release / timeout
                                 ▼
                           ┌───────────┐
                           │  Stopped  │
                           └───────────┘
```

### Brake State Detayları

**Brake Nasıl Çalışır:**
- Tüm high-side MOSFET'ler kapatılır (PWM = 0)
- Tüm low-side MOSFET'ler açılır (digitalWrite HIGH)
- Motor EMF'si low-side MOSFET'ler üzerinden kısa devre yapar
- Motor kontrollü yavaşlar

**Brake Süresi:**
- `brakeHoldMs` — maksimum brake süresi (default: 500ms)
- Bu süre sonunda otomatik olarak Stopped'a geçer
- Kullanıcı brake sırasında `s` gönderirse anında coast'a geçer

**Brake Release Koşulları:**
- `s` komutu gelirse → Stopped (coast)
- `brakeHoldMs` timeout → Stopped (coast)
- Watchdog timeout → Stopped (coast)
- Fault → Stopped (coast)

**Yapılacak Teknik İşler:**
- [ ] [brake] `MotorPhase::Braking` enum ekle
- [ ] [brake] `brakeEnabled` config ekle (EEPROM)
- [ ] [brake] `brakeHoldMs` config ekle (EEPROM)
- [ ] [brake] `brakeAllLowSide()` fonksiyonu yaz
- [ ] [brake] `beginBrake()` fonksiyonu yaz
- [ ] [brake] `k` komutunu `processCommand()`'a ekle
- [ ] [brake] `k` komutunu `processPythonCommand()`'a ekle
- [ ] [brake] State machine'e Braking fazını ekle
- [ ] [brake] Brake timeout mekanizması ekle
- [ ] [brake] Brake sırasında `s` komutunu handle et (coast'a geç)
- [ ] [brake] Telemetri'ye brake durumunu ekle
- [ ] [brake] CLI'ya brake komutlarını ekle (brake on/off, brakehold)

**Etkilenen Modüller:**
- `MotorPhase` enum — main.cpp
- `motorControlTick()` — main.cpp
- `processCommand()` — main.cpp
- `processPythonCommand()` — main.cpp
- `applyDriveState()` — main.cpp (brake durumu)
- `stopMotorImmediate()` — main.cpp (coast olarak kalmalı)
- `SavedConfig` struct — main.cpp

**Riskler:**
- Brake sırasında akım spike'i MOSFET/驱动 hasarına neden olabilir
- Brake süresi çok uzunsa motor ısınabilir
- Low-side short yöntemi motor tipine göre farklı davranabilir

**Başarı Kriterleri:**
- `s` komutu → coast stop (mevcut davranış korunur)
- `k` komutu → brake (aktif fren)
- Brake timeout sonunda motor Stopped'a geçer
- Watchdog/fault durumunda motor coast durur (brake değil)
- Telemetri brake durumunu gösterir

**Bu faz tamamlanmadan sonraki faza geçilebilir mi:** Hayır — brake mimarisinin temeli atılmalı

---

## Phase 5: Brake Güvenlik ve Test Fazı [brake] [safety]

**Amaç:** Brake davranışını güvenli hale getirmek ve donanım üzerinde doğrulamak.

**Neden gerekli:** Brake aktif bir eylem. Akım riskleri var. İlk sürümde reverse torque braking yapılmamalı.

### İlk Güvenli Sürüm Fren Yaklaşımı

**Seçilen Yöntem:** Low-side dynamic brake

| Özellik | Değer |
|---------|-------|
| Yöntem | Tüm low-side MOSFET'leri aç, high-side kapat |
| Akım Kaynağı | Motor EMF (back-EMF) |
| Kontrol | Açık döngü (open-loop) |
| Risk Seviyesi | Düşük-orta |
| Reverse Torque | ❌ İlk sürümde yok |

**Neden Low-Side Dynamic Brake:**
- Reverse torque braking'e göre daha güvenli
- Akım motor EMF'sinden gelir, güç kaynağından değil
- Açık döngü kontrolü basit
- Donanım değişikliği gerektirmez

**Neden Reverse Torque Braking İlk Sürümde Yok:**
- Daha agresif, daha riskli
- Güç kaynağından akım çekilir
- Kontrol döngüsü daha karmaşık
- Gelecekte değerlendirilecek (ISSUES.md'ye not)

### Akım ve Risk Yönetimi

**Brake Sırasında Akım Riski:**
- Motor EMF'si düşük side MOSFET'ler üzerinden kısa devre yapar
- Akım motor hızına ve bobin direncine bağlı
- Yüksek hızda ani brake → yüksek akım spike'i
- `brakeHoldMs` sınırı → akım süresini sınırlar

**Koruma Mekanizmaları:**
- `brakeHoldMs` timeout (default 500ms)
- Brake sırasında `s` komutu → anında coast
- Watchdog/fault → anında coast
- Gelecekte: akım sensörü ile feedback (Phase 7+)

**Yapılacak Teknik İşler:**
- [ ] [brake] [safety] Brake akım testi — düşük duty'de başla, ölçüm yap
- [ ] [brake] [safety] `brakeHoldMs` optimum değerini belirle (donanım testi)
- [ ] [brake] [validation] Brake testi: yüksek hızda ani brake → akım spike ölç
- [ ] [brake] [validation] Brake testi: düşük hızda brake → düzgün durma
- [ ] [brake] [validation] Brake timeout testi → otomatik coast geçiş
- [ ] [brake] [validation] Brake sırasında `s` komutu → anında coast
- [ ] [brake] [validation] Brake sırasında watchdog → anında coast
- [ ] [brake] [safety] Brake telemetrisi ekle (brake aktif/pasif, süre)
- [ ] [brake] [documentation] Brake güvenlik notlarını belgele
- [ ] [brake] [documentation] Brake test sonuçlarını belgele

**Etkilenen Modüller:**
- `applyDriveState()` — main.cpp (brake durumu)
- `motorControlTick()` — main.cpp (brake timeout)
- `sendTelemetry()` — main.cpp (brake durumu)
- ISSUES.md

**Riskler:**
- Yüksek hızda ani brake → MOSFET hasarı riski
- Brake süresi çok uzun → motor ısınması
- Low-side short yöntemi tüm motor tiplerinde düzgün çalışmayabilir

**Başarı Kriterleri:**
- Brake düşük-orta hızda güvenli çalışıyor
- Akım spike'i tolere edilebilir seviyede
- Brake timeout düzgün çalışıyor
- Brake sırasında `s` komutu anında coast'a geçiyor
- Brake telemetrisi doğru gösteriyor

**Bu faz tamamlanmadan sonraki faza geçilebilir mi:** Kısmen — temel brake çalışıyorsa devam edilebilir, ama detaylı test sonuçları olmadan üretim kararı verilmemeli

---

## Phase 6: Telemetry/Protection/Command Cleanup [protocol] [architecture]

**Amaç:** Telemetri, protection ve komut yapısını temizlemek ve tutarlı hale getirmek.

**Neden gerekli:** Brake eklendikten sonra telemetri ve komut yapısı gözden geçirilmeli. 4 motorlu yapıya hazırlık için temiz protokol gerekli.

### Telemetri İyileştirmeleri

**Hedef Telemetri Formatı:**
```
RPM:<val>,D:<duty>,DIR:<F/R>,PH:<phase>,PWM_SET:<val>,PWM_ACT:<val>,BRAKE:<0/1>,FC:<code>,H:<hall>
```

| Field | Açıklama |
|-------|----------|
| RPM | Hesaplanan devir |
| D | Anlık duty cycle |
| DIR | Yön (F/R) |
| PH | Faz (0=Stopped, 1=Kick, 2=Running, 3=NeutralWait, 4=Fault, 5=Braking) |
| PWM_SET | Host tarafından ayarlanan PWM |
| PWM_ACT | Firmware'ın hedef duty'si |
| BRAKE | Brake aktif mi (0/1) |
| FC | Fault kodu |
| H | Ham Hall değeri |

**Yapılacak Teknik İşler:**
- [ ] [protocol] Telemetri formatını güncelle (PWM_SET/PWM_ACT/BRAKE/FC ekle)
- [ ] [protocol] Normal ve Python modlarını birleştir (tek format)
- [ ] [safety] Fault kodu telemetriye ekle
- [ ] [architecture] Command queue'da timestamp ekle (stale detection)
- [ ] [architecture] Latest-command-wins mailbox uygula
- [ ] [documentation] Protokol dokümantasyonunu güncelle

**Etkilenen Modüller:**
- `sendTelemetry()` — main.cpp
- `sendPythonTelemetry()` — main.cpp (birleştirilecek)
- `CommandItem` struct — main.cpp
- `enqueueCommand()` / `dequeueCommand()` — main.cpp
- ARCHITECTURE.md

**Riskler:**
- Telemetri format değişikliği Python host'u bozabilir
- Command queue timestamp ekleme bellek kullanımını artırır

**Başarı Kriterleri:**
- Telemetri formatı tutarlı ve açık
- Fault kodları telemetrde görünür
- Brake durumu telemetrde görünür
- Stale command detection çalışıyor

**Bu faz tamamlanmadan sonraki faza geçilebilir mi:** Evet — bu faz iyileştirme odaklı

---

## Phase 7: 4 Motorlu Genişleme Hazırlığı [multi-motor] [architecture]

**Amaç:** Tek motor state machine'den çok motorlu state machine'e geçiş noktalarını hazırlamak.

**Neden gerekli:** Bu proje tek motor sürücü modülü. 4 motorlu yapı hub STM32 üzerinden olacak. Ama motor sürücü modülü kendisini 4 motorlu yapıya hazırlamalı.

### Mevcut Durum (Tek Motor)

- Tek global `motorRt` ve `hallRt` struct'ları
- Hardcoded pin tanımları
- Protokolde motor ID yok
- Tek UART portu

### Hedef (4 Motor Hub)

- Her motor sürücü kendi STM32'sinde çalışır
- Hub STM32 her sürücüye ayrı UART portundan bağlanır
- Protokol motor-agnostik (her sürücü aynı komutları alır)
- Hub motor ID'yi yönetir

### Hazırlık Adımları

**Motor Sürücü Modülü İçin:**
- [ ] [multi-motor] Protokolü motor-agnostik tut (f/b/s her motor için geçerli)
- [ ] [multi-motor] Global state'i struct içinde tut (zaten mevcut)
- [ ] [multi-motor] Pin tanımlarını config'den oku (gelecekte)
- [ ] [multi-motor] Brake mantığını motor bazlı genelleştir
- [ ] [multi-motor] Telemetri formatını hub-parse edilebilir tut

**Hub STM32 Hazırlığı (Ayrı Proje):**
- [ ] [multi-motor] Hub protokol tasarımı (Python → Hub → Motor)
- [ ] [multi-motor] Sol/sağ taraf komut grupları
- [ ] [multi-motor] Tank turn mantığı
- [ ] [multi-motor] Çok motorlu lease/failsafe
- [ ] [multi-motor] Çok motorlu telemetri ve fault isolation

**Etkilenen Modüller:**
- Tüm main.cpp (motor-agnostik kalmalı)
- Hub STM32 projesi (ayrı repo)

**Riskler:**
- Motor sürücü modülü değişmeden 4 motorlu yapı kurulamaz
- Hub STM32 geliştirme zaman alabilir
- Çok motorlu timing sorunları ortaya çıkabilir

**Başarı Kriterleri:**
- Motor sürücü modülü motor-agnostik kalmış
- Protokol 4 motorlu yapıya uygun
- Hub protokol tasarımı tamamlanmış

**Bu faz tamamlanmadan sonraki faza geçilebilir mi:** Hayır — temel hazırlık tamamlanmalı

---

## Phase 8: Tank Steering ve Multi-Motor Control [multi-motor]

**Amaç:** 4 motorlu skid-steer araç kontrolünü tamamlamak.

**Neden gerekli:** Projenin nihai hedefi 4 motorlu skid-steer araç.

### Skid-Steer Kontrol Mantığı

| Aksiyon | Sol Motorlar | Sağ Motorlar |
|---------|--------------|--------------|
| İleri | f\<duty\> | f\<duty\> |
| Geri | b\<duty\> | b\<duty\> |
| Sol Dönüş | b\<duty\> | f\<duty\> |
| Sağ Dönüş | f\<duty\> | b\<duty\> |
| Spin Sol | b\<duty\> | f\<duty\> |
| Spin Sağ | f\<duty\> | b\<duty\> |
| Dur (coast) | s | s |
| Dur (brake) | k | k |

### Yapılacak Teknik İşler

- [ ] [multi-motor] Hub STM32 projesi başlat
- [ ] [multi-motor] Python host 4 motor desteği ekle
- [ ] [multi-motor] Skid-steer kontrol mantığı uygula
- [ ] [multi-motor] Tank turn modları (arcade/tank)
- [ ] [multi-motor] 4 motorlu telemetri dashboard
- [ ] [multi-motor] Fault isolation (bir motor bozulursa diğerleri)
- [ ] [multi-motor] 4 motorlu brake koordinasyonu
- [ ] [validation] 4 motorlu entegrasyon testleri

**Etkilenen Modüller:**
- Hub STM32 projesi (yeni repo)
- Python host (güncelleme)
- Motor sürücü modülü (test)

**Riskler:**
- Hub STM32 geliştirme zaman alabilir
- 4 motorlu timing sorunları
- Güç kaynağı gereksinimleri
- Brake koordinasyonu karmaşık olabilir

**Başarı Kriterleri:**
- 4 motor eş zamanlı kontrol edilebiliyor
- Skid-steer dönüş düzgün çalışıyor
- Herhangi bir motor fault'ünde diğerleri güvenli duruyor
- 4 motorlu telemetri dashboard çalışıyor

**Bu faz tamamlanmadan sonraki faza geçilebilir mi:** Bu son faz

---

## Faz Bağımlılık Grafiği

```
Phase 0 (Belgeleme)
    │
    ▼
Phase 1 (Stabilize)
    │
    ▼
Phase 2 (Protokol)
    │
    ▼
Phase 3 (Lease/Watchdog)
    │
    ▼
Phase 4 (Stop/Brake Ayrımı)
    │
    ▼
Phase 5 (Brake Güvenlik)
    │
    ▼
Phase 6 (Cleanup)
    │
    ▼
Phase 7 (Multi-Motor Hazırlık)
    │
    ▼
Phase 8 (Tank Steering)
```

---

## Mevcut Durum

| Faz | Durum | Notlar |
|-----|-------|--------|
| Phase 0 | Tamamlandı | Mevcut yapıları belgeleme |
| Phase 1 | Tamamlandı | Queue, saveall, identify, default PWM, telemetry, timestamp, mailbox |
| Phase 2 | Tamamlandı | f/b/s protokolü uygulandı, WASD kaldırıldı |
| Phase 3 | Tamamlandı | Lease/watchdog, Python watchdog aktif, lastMotorCommandMs tutarlılığı, host connection monitor |
| Phase 4 | Başlamadı | Stop/brake ayrımı |
| Phase 5 | Başlamadı | Brake test |
| Phase 6 | Başlamadı | Temizlik |
| Phase 7 | Başlamadı | Multi-motor hazırlık |
| Phase 8 | Başlamadı | Tank steering |

---

## Bu Değişiklikler Neden Sadece Feature Değil, Mimari Karar?

### Timer Tabanlı Asenkron Yapı

Mevcut kodda `runMotorControlScheduler()` 60µs tick ile motor kontrolünü zamanlıyor. `hallISR()` sadece pin okuma ve flag set ediyor (hafif). UART işleme ana döngüde motor kontrolünden sonra geliyor.

**Neden mimari karar:** Bu ayrım korunmazsa:
- 4 motorlu yapıda timing garantisi verilemez
- UART parsing motor komütasyonunu geciktirebilir
- Motor kontrol hot path'i predictability kaybeder

### Stop vs Brake Ayrımı

Mevcut `stopMotorImmediate()` → `allOff()` = coast. Brake yok.

**Neden mimari karar:**
- Brake aktif bir eylem (akım tüketir, MOSFET sürer)
- Watchdog/fault durumlarında default = güvenli durum = coast
- Brake sadece kullanıcı kontrollü, bilinçli bir eylem olmalı
- Bu ayrım yapılmazsa: watchdog timeout'ta motor aniden fren yapabilir (istenmeyen)

### Multi-Motor Geleceğine Etkisi

Motor sürücü modülü motor-agnostik kalmalı. Hub STM32 motor ID'yi yönetir. Protokol f/b/s her motor için geçerli.

**Neden mimari karar:**
- Motor sürücü modülü değişmeden 4 motorlu yapı kurulabilir
- Hub STM32 motor ID'yi ekler, motor sürücü bilmez
- Brake mantığı motor bazlı genelleştirilebilir

---

## İlk Uygulanacak 12 Teknik İş

| # | İş | Faz | Etiket | Öncelik |
|---|-----|-----|--------|---------|
| 1 | Kuyruk if→while düzeltmesi | Phase 1 | [bugfix] | Kritik |
| 2 | saveall mod kaydetme | Phase 1 | [bugfix] | Kritik |
| 3 | IDENTIFY_TOGGLE_MS artırma | Phase 1 | [bugfix] | Yüksek |
| 4 | Default PWM yapılandırılabilir | Phase 1 | [bugfix] | Yüksek |
| 5 | Telemetri PWM_SET/PWM_ACT | Phase 1 | [architecture] | Orta |
| 6 | f/b/s protokol parsing | Phase 2 | [protocol] | Kritik |
| 7 | Python watchdog aktifleştirme | Phase 3 | [safety] | Kritik |
| 8 | Lease kurallarını belgele | Phase 3 | [safety] | Yüksek |
| 9 | MotorPhase::Braking ekle | Phase 4 | [brake] | Yüksek |
| 10 | brakeAllLowSide() fonksiyonu | Phase 4 | [brake] | Yüksek |
| 11 | Brake akım testi | Phase 5 | [brake] [validation] | Orta |
| 12 | Hub protokol tasarımı | Phase 7 | [multi-motor] | Orta |
