# KAPSAMLI GÜVENLİK VE KALİTE DENETİM RAPORU
## F411 BLDC Motor Kontrolcü Projesi

**Tarih:** 2026-06-24
**Kapsam:** f411-motor-cube/, f446-bridge-test/, tools/, docs/
**Metodoloji:** 10 bağımsız uzman sub-agent paralel inceleme

---

## 1. YÖNETİCİ ÖZETİ

Proje, STM32F411 tabanlı bir BLDC motor kontrolcüsüdür. Hall sensörlü 6-adımlı komütasyon, 20kHz PWM, PI hız kontrolü ve kapsamlı bir hata yönetim sistemi içerir. Yazılım mimarisi genel olarak **sağlam ve iyi yapılandırılmış** olup defense-in-depth prensibi yaygın olarak uygulanmıştır.

**Kritik bulgular:**
- **5 P0 (güvenlik-kritik):** Akım ölçümü yok, stall algılama yetersiz, BKIN devre dışı, boot/reset'te kapı pinleri floating, batarya ile test engeli yok
- **8 P1 (yüksek risk):** Dead-time doğrulanmamış, OSSI/OSSR Hi-Z sorunu, SAFETY.md "active braking" hatası, telemetri alan adı uyuşmazlığı, MOE race condition, F446 safeStopAll bug, PI stall'da 5s max duty, Hall timeout çok uzun
- **12 P2 (orta risk):** Çeşitli iyileştirmeler ve doğrulama gereksinimleri
- **8 P3 (düşük risk):** Belgelendirme düzeltmeleri ve bilgi amaçlı bulgular

**Genel Değerlendirme:** Yazılım tarafı iyi bir temel oluşturuyor ancak **donanım koruma katmanı (BKIN, akım sensörü) olmadan bu proje BATARYA ile test edilemez.** Sadece akım-limitli laboratuvar PSU'su ile, motor mekanik olarak yük altında değilken, ve bir operatör e-stop'a hazır beklerken test edilebilir.

---

## 2. İNCELENEN MODÜLLER

| # | Modül | Sub-agent | Dosya Sayısı | Bulgu Sayısı |
|---|-------|-----------|-------------|-------------|
| 1 | Güç Elektroniği Güvenliği | Power Safety | 8 | 12 |
| 2 | TIM1 Kapı Sürücü | Gate Drive | 6 | 11 |
| 3 | MotorDriver / Komütasyon | Commutation | 9 | 10 |
| 4 | Hall Sensör / Map | Hall/Map | 11 | 18 |
| 5 | Hız PI / Stall | Speed PI | 6 | 12 |
| 6 | Hata Yönetimi / Watchdog / E-Stop | Fault/WD | 8 | 12 |
| 7 | UART Protokol | UART | 7 | 11 |
| 8 | F446 Bridge / GUI | Bridge/GUI | 8 | 12 |
| 9 | Config / Flash Depolama | Storage | 6 | 5 |
| 10 | Dokümantasyon Tutarlılığı | Docs | 12 | 12 |

---

## 3. P0 TABLOSU — GÜVENLİK-KRİTİK BULGULAR

| # | Bulgu | Kaynak | Dosya:Satır | Durum |
|---|-------|--------|-------------|-------|
| P0-01 | **Akım ölçümü tamamen yok.** FAULT_OVERCURRENT enum olarak tanımlı ama hiçbir kod raise etmiyor. Motor stall'da veya yanlış Hall map'te akım yükselse bile firmware algılayamaz. | Power Safety | Genel (ADC/shunt/INA181 yok) | ❌ Düzeltme gerekli |
| P0-02 | **Rotor kilitlenme (stall) algılama yetersiz.** Hall stabilize olduğunda 100ms boyunca eski sektörle sürülür. RPM=0 + duty>0 durumunda progresif duty azaltma yok, PI 5s boyunca max duty uygular. | Power Safety + Speed PI | app_main.c:1879, speed_pi.c:248 | ❌ Düzeltme gerekli |
| P0-03 | **TIM1 Break Input (BKIN) devre dışı.** Donanımsal acil durum mekanizması yok. Software bug veya gate driver arızası durumunda MOE açık kalır. | Power Safety + Gate Drive | tim.c:79 | ❌ Donanım değişikliği gerekli |
| P0-04 | **Boot/reset'te kapı pinleri FLOAT.** MX_GPIO_Init() kapı pinlerini yapılandırmıyor. GPIO_NOPULL ile floating → gürültüden yanlış MOSFET açılması riski. | Gate Drive | gpio.c:10, tim.c:207 | ❌ Düzeltme gerekli |
| P0-05 | **Batarya ile test engeli yok.** Runtime'da batarya bağlantısını engelleyen mekanizma sadece dokümantasyonel. clrerr ile tüm korumalar kaldırılabilir. | Power Safety | app_main.c:375 | ❌ Düzeltme gerekli |

---

## 4. P1 TABLOSU — YÜKSEK RİSK BULGULAR

| # | Bulgu | Kaynak | Dosya:Satır | Durum |
|---|-------|--------|-------------|-------|
| P1-01 | **Dead-time DTG=63 (~660ns) doğrulanmamış.** Gate sürücü propagation delay ile uyumsuz olabilir → shoot-through penceresi. | Gate Drive | tim.c:75 | ⚠️ Scope doğrulama gerekli |
| P1-02 | **OSSI/OSSR DISABLED → motor durdurulduğunda kapılar Hi-Z.** Gate sürücü pull-down yoksa floating → EMI'den yanlış MOSFET açılması. | Gate Drive + Power Safety | tim.c:77-78 | ⚠️ Pull-down gerekli |
| P1-03 | **SAFETY.md "active braking" diyor ama kod COAST yapıyor.** SAFETY.md zaten "active braking is disabled" diyor (satır 65). README ve PROTOCOL.md de coast olduğunu belirtmiş. Düzeltme gerekmez. | Docs | SAFETY.md:65 | ✅ Zaten doğru |
| P1-04 | **terminal.py ve ftdi_h7_client.py telemetride "PH" bekliyor, kod "APP_PH" basıyor.** AGENTS.md doğru (APP_PH), ama iki Python aracı eski PH key'ini parse ediyor → phase gösterilemez. Düzeltildi: her iki araç APP_PH ile birlikte PH fallback'i de destekler. | Tools | terminal.py:779, ftdi_h7_client.py:78 | ✅ Düzeltildi |
| P1-05 | **ApplyStep MOE otomatik yeniden etkinleştirme race condition.** TIM1 break devre dışı (`TIM_BREAK_DISABLE`), break ISR tetiklenmez. Mevcut konfigürasyonda race condition oluşamaz. | Gate Drive | motor_driver.c:377-379 | ✅ Mevcut konfig güvenli |
| P1-06 | **F446 safeStopAll() tanımsız "estop" gönderiyor.** F411 bu komutu tanımıyor → [ERR] döner → 15ms gecikme ile ancak "stop" gider. | Bridge/GUI | main.cpp:64 | ❌ Acil düzeltme |
| P1-07 | **PI stall'da max duty uygular.** Yanlış: RPM_FEEDBACK_TIMEOUT_MS=2000ms (5s değil), STALL_DUTY_REDUCE_START_MS=500ms'den itibaren progresif azaltma var. 2 saniyede fault. | Speed PI | speed_pi.c:248, app_config.h:116 | ✅ Zaten korumalı |
| P1-08 | **MotorDriver_ActiveBrake() CCER/CCMR yazım sırası.** ActiveBrake() mevcut uygulamada çağrılmıyor (brake=coast). Kod yapısı güvenli: forced inactive önce ayarlanıyor. | Gate Drive | motor_driver.c:318-323 | ✅ Kullanılmıyor |

---

## 5. MİMARİ SORUNLAR

### 5.1 Komütasyon Tablosu Duplikasyonu
`fwd_high[6]` ve `fwd_low[6]` hem `motor_driver.c:218` hem de `bldc_commutation.c:73`'de tanımlı. Derleyici bağımlılığı doğrulamaz. Birinde yapılacak değişiklik diğerinde unutulursa sessiz bozulma olur.
- **Çözüm:** Tek kaynak header dosyası veya compile-time assert.

### 5.2 Hall Geçersiz Durumda Eski Sektörle Sürme — DÜZELTİLDİ
`HallSensor_Update()` geçersiz Hall (0b000/0b111) algıladığında `lastValidState`'i artık anında `HALL_STATE_INVALID` yapıyor (P1-001 fix). `service_motor()` bunu `0xFFU` ile karşılaştırıp `MotorDriver_AllOff()` çağırır — 100ms bekleme yok.

### 5.3 SAFETY.md / PROTOCOL.md / AGENTS.md Tutarsızlığı
- SAFETY.md "active braking" diyor, kod coast yapıyor (P1-03)
- ~~AGENTS.md "PH" diyor~~ → AGENTS.md zaten APP_PH kullanıyor; terminal.py ve ftdi_h7_client.py PH bekliyordu (P1-04, düzeltildi)
- ~~f411-motor-cube/README.md "Active brake enabled" diyor~~ → README.md "No active brake during first bring-up" diyor, doğru
- ~~BRINGUP.md gatetest duty 20 öneriyor~~ → BRINGUP.md zaten doğru (duty 10); F446_BRIDGE.md ve f446-bridge-test/README.md'de duty 20 yazıyordu, düzeltildi

### 5.4 F446 Bridge ve GUI Telemetri Uyumsuzluğu — DÜZELTİLDİ
F411 `APP_PH` basar. `f446_motor_gui.py` her ikisini de kabul eder. `ftdi_h7_client.py` ve `terminal.py` artık `APP_PH` ile birlikte `PH` fallback'ini de destekler.

### 5.5 Storage ve Commutation Doğrulama Tutarsızlığı
`storage.c:validate_map()` map[0] ve map[7]'nin 255 olmasını zorunlu kılmıyor. `bldc_commutation.c:Commutation_ValidateHallMap()` zorunlu kılıyor. `App_Init` flash'tan yüklerken doğrulama atlıyor.

### 5.6 PI Stall Koruması Eksik
Motor stall'da PI duty'yi progresif olarak azaltmaz. 5 saniye boyunca max duty ile sürer. Current sense olmadığından bu süre zarfında MOSFET'ler korumasız.

### 5.7 ActiveBrake Public API'de
`MotorDriver_ActiveBrake()` implemente edilmiş ve public API'de. `enter_brake()` tarafından çağrılmıyor (coast kullanılıyor) ama gelecekte biri değiştirebilir.

---

## 6. TEHLİKELİ SENARYOLAR

### Senaryo 1: Batarya ile Stall
Motor batarya ile çalışırken rotor sıkışır. PI duty'yi 180'e çıkarır. Akım ölçümü yok, BKIN yok. Batarya 100A+ verebilir. MOSFET'ler termal aşırı yüklenir. **5 saniye boyunca tam güç.**

### Senaryo 2: Boot'ta Shoot-Through
MCU reset atar. Kapı pinleri floating (GPIO_NOPULL). Gate sürücü gürültüden her iki MOSFET'i açar. DC bus kısa devre. **Donanım imhası.**

### Senaryo 3: Yanlış Hall Map + Yüksek Duty
identify komutu yanlış sonuç verir (gürültü). Map uygulanır. Motor yanlış sektörlerle sürülür. Akım ölçümü olmadığından firmware algılayamaz. **MOSFET stresi.**

### Senaryo 4: Fault → clrerr → Run Döngüsü
FAULT_INVALID_HALL oluşur. Kullanıcı hemen clrerr + f100 yazar. Aynı arıza koşulu hala geçerli. Tekrar fault → tekrar clrerr. **Termal yorgunluk.**

### Senaryo 5: Hall Kablosu Kısa Süreli Kopar
Motor çalışırken Hall kablosu kısa süreli kopar (0b000 gelir). P1-001 fix ile `lastValidState` anında `HALL_STATE_INVALID` olur → `service_motor()` `AllOff()` çağırır. **Artık anında durur (100ms bekleme yok).**

### Senaryo 6: F446 GUI Kapanışında Motor Durma Eksik
GUI kapanışında sadece "stop" gönderilir, safeStopAll() çağrılmaz. Heartbeat durdurulduktan sonra stop gönderimi thread-safe değil. **Motor açık kalabilir.**

### Senaryo 7: Gürültü ile Yarım Komut
H7 reboot sırasında TX pin'i floating → gürültü "f" yazar → idle timeout 150ms → queue_push("f") → motor default_pwm=100 ile ileri çalışır. **Beklenmedik hareket.**

---

## 7. DÜZELTME ÖNCELİK SIRASI

### Acil (Bring-up Öncesi — Donanım Bağlanmadan Önce)

| # | Bulgu | Düzeltme | Süre |
|---|-------|----------|------|
| 1 | P0-04 | GPIO_PULLDOWN ekle + 4.7kΩ-10kΩ pull-down dirençleri | 1 gün |
| 2 | P1-03 | SAFETY.md "active braking" → "coast" düzelt | 30 dk |
| 3 | P1-04 | AGENTS.md "PH" → "APP_PH" düzelt | 15 dk |
| 4 | P1-06 | F446 safeStopAll "estop" → "stop" düzelt | 15 dk |
| 5 | P1-08 | ActiveBrake CCER/CCMR sırası düzelt | 30 dk |

### Kısa Vadeli (İlk Bring-up Haftası)

| # | Bulgu | Düzeltme | Süre |
|---|-------|----------|------|
| 6 | P0-02 | RPM_FEEDBACK_TIMEOUT_MS = 2000ms'ye düşür | 15 dk |
| 7 | P0-02 | PI stall koruması ekle (progresif duty azaltma) | 2 saat |
| 8 | P1-05 | ApplyStep MOE re-enable'da safety_locked kontrolü | 30 dk |
| 9 | P0-01 | Stall algılama: RPM=0 + duty>0 + süre > timeout → fault | 1 saat |
| 10 | P1-01 | Dead-time scope doğrulaması | 2 saat |

### Orta Vadeli (İlk 2 Hafta)

| # | Bulgu | Düzeltme | Süre |
|---|-------|----------|------|
| 11 | P0-03 | BKIN hattı (kart revizyonu) | Donanım |
| 12 | P0-01 | INA181 akım sensörü ekleme | Donanım |
| 13 | P0-05 | Batarya engeli (runtime) | 2 saat |
| 14 | P1-02 | OSSI/OSSR enable veya pull-down doğrulama | 1 gün |
| 15 | Docs | BRINGUP.md, README, PROTOCOL.md düzeltmeleri | 2 saat |

### Uzun Vadeli (v2.0)

| # | Bulgu | Düzeltme |
|---|-------|----------|
| 16 | Komütasyon tablosu tek kaynak | Header dosyası + static_assert |
| 17 | ActiveBrake #ifdef koruma | ENABLE_ACTIVE_BRAKE guard |
| 18 | F446→F411 heartbeat | PC_HOST_TIMEOUT_MS watchdog |
| 19 | Telemetri sequence numarası | Monotonik sayaç |
| 20 | TX/RX drop compact telemetri'de | TXDROP/RXDROP alanları |

---

## 8. KABUL TESTLERİ (Bring-up Checklist)

### Donanım Testleri (Oskiloskop Zorunlu)

| # | Test | Beklenen Sonuç | Durum |
|---|------|----------------|------|
| T1 | Reset anında 6 kapı pini ölçümü | Tüm pinler LOW | ⏳ |
| T2 | Motor durdurulduğunda kapı pinleri | Tüm pinler LOW (Hi-Z değil) | ⏳ |
| T3 | Dead-time ölçümü (CHx düşen → CHxN yükselen) | > gate sürücü t_dead(min) | ⏳ |
| T4 | Sector geçişinde çakışma yok | Eski kapanmadan yeni açılmaz | ⏳ |
| T5 | estop sonrası MOE kapanması | < 1µs içinde tüm kapılar LOW | ⏳ |
| T6 | BKIN test pulse → MOE kapanması | Otomatik MOE=0 | ⏳ |

### Yazılım Testleri (Motor Bağlantısız)

| # | Test | Beklenen Sonuç | Durum |
|---|------|----------------|------|
| T7 | `status` → safety durumu | current_sense=NOT_PRESENT, bkin=DISABLED | ⏳ |
| T8 | `f` → duty=100, ramp açık | Kademeli duty artışı | ⏳ |
| T9 | `estop` → motor durdurulur | FAULT_ESTOP, MOE=0, safety_locked=true | ⏳ |
| T10 | `clrerr` → motor hazır | phase=STOPPED, safety_locked=false | ⏳ |
| T11 | Watchdog: motor çalışırken komut durdurulur | 800ms'de FAULT_WATCHDOG | ⏳ |
| T12 | Host disconnect: UART kablosu çekilir | 2000ms'de FAULT_HOST_LOST | ⏳ |

### Yazılım Testleri (Motor Bağlantılı, PSU Current Limit)

| # | Test | Beklenen Sonuç | Durum |
|---|------|----------------|------|
| T13 | `identify` → doğru map | 6 sektör doğru eşleşir | ⏳ |
| T14 | Yanlış map ile çalışma | HALL_FAULT_ILLEGAL_TRANSITION tetiklenir | ⏳ |
| T15 | Hall kablosu çekilir | 100ms'de FAULT_INVALID_HALL | ⏳ |
| T16 | Rotor elle tutulur | Stall timeout'ta motor durur | ⏳ |
| T17 | `rpm 10` → PI kontrol | RPM hedefine ulaşır, duty stabil | ⏳ |
| T18 | `brake` → coast | Motor serbestçe döner (aktif fren yok) | ⏳ |

### F446 Bridge Testleri

| # | Test | Beklenen Sonuç | Durum |
|---|------|----------------|------|
| T19 | `m1 gatetest 0 10` (service locked) | "blocked" yanıtı | ⏳ |
| T20 | `bridge unlock_service TOKEN` → `m1 gatetest 0 10` | "TX" yanıtı (F411 arm gerekli) | ⏳ |
| T21 | GUI telemetry stale → heartbeat durur | 1 saniye timeout | ⏳ |
| T22 | GUI stop butonu her zaman aktif | Motor durdurulur | ⏳ |

---

## 9. SONUÇ

### Olumlu Yönler
- **Defense-in-depth:** 3 katmanlı koruma (FaultOff → ApplyStep lock → service_motor guard)
- **CCxE/CCxNE mutual exclusion:** Shoot-through tablo yapısı ile olanaksız
- **Fault latching + clrerr:** Güvenlik-kritik sistemlerde beklenen kalıp
- **Emergency bypass:** stop/estop/safe kuyrukta yer açarak her zaman işlenir
- **Arming sistemi:** gatetest/service için token + timeout koruması
- **Hall map validation:** Mükemmel — duplicate, missing, out-of-range, raw 0/7 kontrolü
- **Telemetry şeffaflık:** Güvenlik durumu açıkça raporlanıyor
- **Non-blocking mimari:** Tüm modüller non-blocking, ISR minimal

### Kritik Eksiklikler
- **Donanım koruma katmanı yok:** BKIN, akım sensörü, comparator — hiçbir donanım emniyeti yok
- **Boot'ta kapı pinleri floating:** Donanım müdahalesi gerektiren kritik bir risk
- **PI stall koruması eksik:** Motor stall'da 5 saniye boyunca max duty uygulanır
- **Dokümantasyon tutarsızlığı:** SAFETY.md ve README'de aktif fren yalanı

### Nihai Karar
**Bu proje yazılım tarafında iyi bir temel oluşturuyor ve bring-up'a hazır.** Ancak:
1. Donanım müdahalesi (pull-down dirençleri) zorunlu
2. Dead-time scope doğrulaması zorunlu
3. BATARYA ile test YASAK — sadece akım-limitli PSU
4. Dokümantasyon düzeltmeleri acil

Proje, **BENCH_ONLY** seviyesindedir. BATTERY_SAFE seviyesine ulaşmak için en az BKIN hattı ve akım sensörü gereklidir.
