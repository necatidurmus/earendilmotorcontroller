# Earendil Motor Controller — Hedef Ağacı ve Yol Haritası

## Kullanıcı Sorunları ile Mevcut Known Issues Karşılaştırması

Kullanıcı 3 kategoride sorun sundu. Aşağıda her madde için kod doğrulama sonucu ve known_issues.md ile eşleşme durumu var.

### 1-A. Derleme ve Arayüz Tutarsızlıkları

| # | Kullanıcı Sorunu | Kod Doğrulama | Known Issues'te Var mı? | Aksiyon |
|---|---|---|---|---|
| A1 | huart2 tekrar tanımı | **Sorun yok** — tek definition (board_io.c:36), cli.c:48'deki extern gereksiz ama zararsız | Yok | Müdü gereksiz extern satırı silinebilir (LOW priority) |
| A2 | HallSnapshot alan adı tutarsızlığı | **Sorun yok** — tüm dosyalarda `driveState` tutarlı | Yok | Aksiyon gerekmiyor |
| A3 | UART açıklamaları USART1 diyor | **Sorun yok** — cli.h ve board_io.h ikisi de USART2 diyor, daha önce düzeltildi | ✅ #2'de düzeltildi olarak işaretli | Aksiyon gerekmiyor |
| A4 | RunMode ayrı header'a alınmalı | **Zaten yapılmış** — RunMode sadece bldc_commutation.h:40-44'de tanımlı, main.c ve cli.c extern kullanıyor | Yok | Aksiyon gerekmiyor |

### 1-B. Mantık ve Semantik Tutarsızlıkları

| # | Kullanıcı Sorunu | Kod Doğrulama | Known Issues'te Var mı? | Aksiyon |
|---|---|---|---|---|
| B1 | Hall_SetDirection() semantiği net değil | **GERÇEK SORUN** — hall.h:62 yorumu "0=forward, 1=backward" diyor, kod `forward ? 0 : 1` ile tersini yapıyor. main.c:121 `Hall_SetDirection(1)` "forward by default" ile çağırıyor, bu doğru. Yorum yanlış. | Yok | Yorum düzeltilecek: "1=forward (state as-is), 0=backward (state+3)" |
| B2 | Yön mantığı iki yerde kurulmuş | **KISMEN DOĞRU** — `Hall_GetDriveState()` hall.c'de var ama ISR'da kullanılmıyor (ölü kod). Asıl yön bldc_commutation.c'de CCER tablosuyla `Comm_ApplyStep()` üzerinden yapılıyor. Tek direction authority: `Comm_ApplyStep()` + `RunMode`. | Yok | `Hall_GetDriveState()` ölü kod olarak işaretlenebilir veya silinebilir |
| B3 | Sürüş tipi asenkron 6-step diyor | **YANLIŞ TESPİT** — bldc_commutation.c açıkça senkron komplementer PWM (high-side TIM1_CHx PWM + low-side TIM1_CHxN complementary, hardware deadtime). CCER tablosunda her adımda CH + CHN aktif. | Yok (known_issues'da "synchronous complementary" doğru yazılmış) | Kullanıcı bilgisi güncellenecek |
| B4 | HardFault yorumları gerçek davranışla hizalanmalı | **Sorun yok** — stm32f4xx_it.c:27-36 yorum "turn off all outputs" diyor, kod CCER=0 + CCR1/2/3=0 ile gerçekten kapatıyor | ✅ #1'de düzeltildi olarak işaretli | Aksiyon gerekmiyor |

### 1-C. Zamanlama ve Konfigürasyon Hataları

| # | Kullanıcı Sorunu | Kod Doğrulama | Known Issues'te Var mı? | Aksiyon |
|---|---|---|---|---|
| C1 | PWM frekansı hesapları yeniden doğrulanmalı | **GERÇEK SORUN** — motor_config.h yorumları 100 MHz değerler kullanıyor, gerçek 96 MHz. APB2 timer clock yorumu karışık. | ✅ #15 (HIGH) | Düzeltilecek |
| C2 | USB clock PLLQ uygun değil | **Sorun yok** — PLLQ=4, VCO=192 MHz, USB=192/4=48 MHz. USB FS için doğru. | Yok | Aksiyon gerekmiyor |
| C3 | Dead-time yaklaşımı kaba | **Doğru tespit** — bir tam ISR periyodu all-off state geçişinde uygulanıyor (slew). Bu güvenli ama kaba. | Yok (known_issues'da yok) | Dokümantasyona eklenebilir (LOW) |
| C4 | ISR içinde blocking ADC okuma | **GERÇEK SORUN** — BoardIO_ReadADC() ISR içinde blocking while(EOC) polling, timeout yok. | ✅ #9 (MEDIUM) — ama kullanıcı daha yüksek öncelik veriyor | Öncelik yükseltilecek |
| C5 | Clock-tree USB CDC için uygun değil | **Yanlış değerlendirme** — PLLQ=4 doğru, USB 48 MHz veriyor. | Yok | Aksiyon gerekmiyor |

### 1-D. Repo Hijyeni ve Dokümantasyon

| # | Kullanıcı Sorunu | Kod Doğrulama | Aksiyon |
|---|---|---|---|
| D1 | Legacy dosyalar ayrılmalı | src/ içinde .cpp yok, tüm kaynaklar .c. Ama motor_config.h yorumlarında Arduino framework değerleri var. | Yorum temizliği yapılacak (#16) |
| D2 | README gerçek durumu anlatmalı | Mevcut README'yi kontrol etmek gerek | README güncellenecek |

---

## Kullanıcı Sorunlarından Yeni Eklenen Known Issues

Aşağıdaki sorunlar mevcut known_issues.md'de **yoktu** ama kullanıcı tarafından tespit edildi:

| # | Yeni Sorun | Şiddet | Açıklama |
|---|---|---|---|
| N1 | Hall_SetDirection yorum-kod çelişkisi | MEDIUM | hall.h:62 yorumu "0=forward" diyor, kod tersini yapıyor |
| N2 | Hall_GetDriveState() ölü kod | LOW | hall.c'de tanımlı ama ISR'da hiç çağrılmıyor, yön mantığı bldc_commutation.c'de |
| N3 | Dead-time state geçişi kaba (slew all-off) | LOW | Bir tam ISR periyodu all-off, belgelenmeli |
| N4 | cli.c'de gereksiz extern huart2 | LOW | motor_config.h zaten extern veriyor, cli.c:48 duplicate |

---

## Hedef Ağacı (Öncelik Sırasıyla)

### ~~Phase 0: Masa Başı Düzeltmeler (Ölçüm Gerektirmez)~~ ✅ Tamamlandı

| Sıra | Sorun | Durum |
|---|---|---|
| ~~0.1~~ | ~~SysTick öncelik çakışması~~ | ✅ TICK_INT_PRIORITY=15 |
| ~~0.2~~ | ~~Volatile eksikliği~~ | ✅ ISR-shared değişkenlere volatile eklendi |
| ~~0.3~~ | ~~INT32_MIN UB~~ | ✅ Overflow-safe negation |
| ~~0.4~~ | ~~nowUs overflow~~ | ✅ 64-bit multiply |
| ~~0.5~~ | ~~ADC EOC timeout~~ | ✅ ~10 us timeout |
| ~~0.6~~ | ~~cliPrintFloat overflow~~ | ✅ buf[16] |
| ~~0.7~~ | ~~EMA filtre init~~ | ✅ filterInitialized flag |
| ~~0.8~~ | ~~gain help uyumsuz~~ | ✅ "1..1000" |
| ~~0.9~~ | ~~CCER direkt atama~~ | ✅ CCER_COMM_MASK RMW |
| ~~0.10~~ | ~~FaultState dead code~~ | ✅ Silindi |
| ~~0.11~~ | ~~Hall_SetDirection yorum~~ | ✅ Düzeltildi |
| ~~0.12~~ | ~~Hall_GetDriveState ölü kod~~ | ✅ "diagnostic only" |
| ~~0.13~~ | ~~cli.c gereksiz extern~~ | ✅ Silindi |
| ~~0.14~~ | ~~platformio.ini build_unflags~~ | ✅ Temizlendi |
| ~~0.15~~ | ~~MOE runtime check~~ | ✅ Assert eklendi |
| ~~0.16~~ | ~~motor_config.h clock yorumları~~ | ✅ 96 MHz |
| ~~0.17~~ | ~~board_io.c ADC clock yorumları~~ | ✅ PCLK2/4 = 24 MHz |
| ~~0.18~~ | ~~board_io.h SYSCLK yorumu~~ | ✅ "96 MHz" |
| ~~0.19~~ | ~~known_issues.md outdated~~ | ✅ Güncellendi |
| ~~0.20~~ | ~~Prot_ClearFault null-term~~ | ✅ Explicit '\0' |
| ~~0.21~~ | ~~Hall pin speed~~ | ✅ GPIO_SPEED_FREQ_LOW |
| ~~0.22~~ | ~~RCC GPIOA tekrarlı~~ | ✅ Duplicate kaldırıldı |
| ~~0.23~~ | ~~Dead-time slew belgeleme~~ | ✅ control_strategy.md'ye eklendi |

### Phase 1: Ölçüm Gerektiren — Konfigürasyon Kalibrasyonu

Ölçüm yapıldıktan sonra `motor_config.h` ve `protection.c`'deki sabitler güncellenecek. **Tüm değerler config dosyasında merkezi tutulacak, değiştirmesi kolay olacak.**

| Sıra | Sorun | Kaynak | Ölçüm Gereksinimi | Config Yeri |
|---|---|---|---|---|
| 1.1 | HALL_TO_STATE_PROFILES doğru profil | Kullanıcı B1 | Motor düşük duty'de döndürülecek, hall raw/corrected/mapped/accepted izlenecek | `motor_config.h` HALL_TO_STATE_PROFILES |
| 1.2 | polarityMask | Kullanıcı B1 | Hall hinv/hmask denenecek | `motor_config.h` HALL_POLARITY_MASK |
| 1.3 | stateOffset | Kullanıcı B1 | offset komutu ile denenecek | Runtime `offset` komutu |
| 1.4 | Faz-hall eşleşme doğrulama | Kullanıcı B1 | Her step'te CCER ve hall durumu eşzamanlı izlenecek | `motor_config.h` COMMUTATION_TABLE |
| 1.5 | CURRENT_SOFT_LIMIT | Kullanıcı B2 | Boşta dlt not et, x1.5 uygula | `motor_config.h` CURRENT_SOFT_LIMIT |
| 1.6 | CURRENT_HARD_LIMIT | Kullanıcı B2 | Max yükte dlt not et, x0.9 uygula | `motor_config.h` CURRENT_HARD_LIMIT |
| 1.7 | HARD_LIMIT_STRIKES | Kullanıcı B2 | Gürültü davranışına göre 3-5 arası ayarla | `motor_config.h` HARD_LIMIT_STRIKES |
| 1.8 | DUTY_MIN_ACTIVE | Kullanıcı C1 | Motorun güvenli kalktığı en düşük duty | `motor_config.h` DUTY_MIN_ACTIVE |
| 1.9 | DUTY_DEFAULT | Kullanıcı C1 | Kararlı döndüğü duty aralığı | `motor_config.h` DUTY_DEFAULT |
| 1.10 | DUTY_RAMP_UP/DOWN_STEP | Kullanıcı C1 | Akım sıçraması olmadan ramp hızı | `motor_config.h` DUTY_RAMP_* |
| 1.11 | INA181 gain doğrulama | Kullanıcı D1 | PCB suffix oku, bilinen akım geçir, estA karşılaştır | `motor_config.h` INA_GAIN_DEFAULT |
| 1.12 | VSENSE çevirim oranı | Kullanıcı D1 | Multimetre ile voltaj karşılaştır, ratio hesapla | `motor_config.h` VSENSE_DIVIDER_RATIO |
| 1.13 | Offset kalibrasyon kalitesi | Kullanıcı D1 | Tekrarlanan zeroi ile offset sapması ölç | Sabit yok, dokümantasyon |

### Phase 2: Ölçüm Sonrası Mimari Kararlar

| Sıra | Konu | Karar Bekleyen | Koşul |
|---|---|---|---|
| 2.1 | ~~Synchronous complementary mi kalınacak?~~ | — | Zaten uygulanmış, Phase 0'da CCER RMW ile güvenli hale getirildi |
| 2.2 | ADC DMA'ye geçiş | Kullanıcı | Timing gereksinimlerine göre |
| 2.3 | USB CDC'ye geçiş | Kullanıcı | UART bring-up tamamlandıktan sonra |
| 2.4 | TIM1 Break / OCP | Kullanıcı | Donanımda OCP hattı netleşince |

### Phase 3: Güvenlik Eksiklikleri (Uzun Vade)

| Sıra | Konu | Açıklama |
|---|---|---|
| 3.1 | IWDG watchdog | ISR/main loop takılmasına karşı koruma |
| 3.2 | Undervoltage koruma | VSENSE doğrulanınca eklenecek |
| 3.3 | Termal koruma (NTC) | NTC donanımı eklenince |
| ~~3.4~~ | ~~MOE runtime check~~ | ✅ Phase 0'da uygulandı |

---

## Ölçüm Senaryoları

### Senaryo 1: Hall ve Komütasyon Doğrulama

**Ön koşul:** Motor mekanik olarak bağlı, PSU düşük voltaj/akım limitli.

```
1. PSU: 12V, 1A limit
2. Motor elle döndür → hall komutu
   - Beklenen: raw 001→011→010→110→100→101 sıralı (veya profil göre)
   - corrected, mapped, accepted değerlerini kaydet
   - driveState 0→1→2→3→4→5 sıralı olmalı
3. map 0 dene → map 1 → map 2 → map 3
   - Her profil için driveState sıralı mı kontrol et
   - Doğru profili not et
4. hinv 0/1 dene, hmask 0..7 dene
   - Hangi kombinasyonda düzgün çalışıyor not et
5. pwm 8 ile çalıştır, düşük duty'de motor dönüyor mu
   - Dönüyor: doğru profil + polarite
   - Titriyor/dönmüyor: yanlış profil, başka dene
6. offset <-5..5> dene
   - Her offset için hall→state eşleşmesini izle
   - En düzgün çalışan offset'i not et
```

**Kaydedilecek değerler:** map profili, hinv, hmask, offset, doğru hall-to-state tablosu

### Senaryo 2: Akım Kalibrasyonu

**Ön koşul:** Motor bağlı, outputs off, INA181 doğru lehimli.

```
1. zeroi komutu çalıştır → offset değerini kaydet
2. 5 kez tekrarla, offset sapmasını kaydet (±X ADC count)
   - Kabul edilebilir: ±5 ADC count
3. Motor boşta, pwm 8 → current komutu
   - dlt değerini not et (no-load delta)
4. pwm 25 → current → dlt not et
5. pwm 50 → current → dlt not et
6. pwm 70 → current → dlt not et
7. Motoru elle yavaşlat (stall simülasyonu) → current
   - dlt sıçrama miktarını not et
8. Soft limit: no-load dlt × 1.5
9. Hard limit: max kabul edilebilir dlt × 0.9
10. limits <soft> <hard> ile uygula
```

**Kaydedilecek değerler:** offset, offset sapması, no-load dlt (her duty), stall dlt, soft limit, hard limit

### Senaryo 3: Duty ve Ramp Kalibrasyonu

**Ön koşul:** Senaryo 2 tamamlanmış, koruma eşikleri ayarlanmış.

```
1. pwm 5 → motor dönüyor mu? (muhtemelen hayır)
2. pwm 8 → dönüyor mu? (DUTY_MIN_ACTIVE adayı)
3. pwm 10 → dönüyor mu?
4. pwm 15 → dönüyor mu?
5. Her adımda current komutuyla dlt izle
6. Motorun ilk döndüğü duty → DUTY_MIN_ACTIVE
7. Ses/titreme gözlemi:
   - Hangi duty'de anormal ses var?
   - Hangi duty'de titreme var?
   - Bu duty alt sınır (max güvenli duty) olarak not et
8. Akım sıçrama gözlemi:
   - Her duty artışında dlt sıçrama var mı?
   - Hangi duty'de sıçrama kabul edilemez?
9. Ramp testi:
   - start komutu ile ramp-up gözlemle
   - Akım sıçraması olmadan en hızlı ramp-up step?
   - stop komutu ile ramp-down gözlemle
```

**Kaydedilecek değerler:** DUTY_MIN_ACTIVE, max güvenli duty, ramp step limitleri

### Senaryo 4: Analog Ölçekleme Doğrulama

**Ön koşul:** Multimetre, bilinen direnç yükü (opsiyonel).

```
A. INA181 Gain:
   1. PCB'deki suffix oku (A1/A2/A3/A4)
   2. gain <suffix değeri> komutu
   3. Bilinen akım kaynağı varsa: I_known geçir
      → estA komutuyla gösterilen değeri karşılaştır
   4. Gain hatalıysa doğru değeri gain komutuyla ayarla

B. VSENSE:
   1. V komutu ile V raw değerini oku
   2. Multimetre ile gerçek voltajı ölç
   3. Oran = gerçek_voltaj / (V_raw / 4095 * 3.3)
   4. motor_config.h'deki VSENSE_DIVIDER_RATIO ile karşılaştır
   5. Farklıysa config'de güncelle

C. Offset Kalite:
   1. 10 kez zeroi çalıştır
   2. Offset değerlerinin min/max/aralığını hesapla
   3. Kabul edilebilir sapma: ±5 ADC count
```

**Kaydedilecek değerler:** Gerçek INA gain, gerçek VSENSE oranı, offset tekrarlanabilirliği

### Senaryo 5: Güvenli Test Sınırı ve Deadtime Gözlemi

**Ön koşul:** Osiloskop bağlı, düşük voltaj PSU.

```
A. Deadtime Gözlemi:
   1. Osiloskop: PA8 (high) ve PA7 (low) aynı ekranda
   2. pwm 8 ile çalıştır
   3. Deadtime'i ölç (~820-920 ns beklenen)
   4. Cross-conduction var mı kontrol et
   5. Faz B (PA9/PB0) ve Faz C (PA10/PB1) için tekrarla

B. State Geçiş Gözlemi:
   1. Motor dönerken osiloskopla CCER değişimini izle
   2. Hall değişimi → CCER değişimi gecikmesi (<80 us)
   3. State geçişinde all-off periyodu var mı, ne kadar sürer

C. Motor Üzerinde Etki:
   1. Düşük duty'de motor sesi → normal mi?
   2. Deadtime çok yüksekse: verim düşüklüğü, ısınma
   3. State geçiş all-off süresi motor torkuna etkisi
```

**Kaydedilecek değerler:** Ölçülen deadtime, cross-conduction durumu, state geçiş gecikmesi, motor sesi/tork gözlemi

---

## Config Dosyası Yapısı Önerisi

Phase 1 ölçümleri tamamlandıktan sonra, tüm kalibre edilebilir değerler `motor_config.h`'de merkezi tutulacak:

```c
/* === Hall & Komütasyon (ölçümle belirlenecek) === */
#define HALL_ACTIVE_PROFILE     0       // Senaryo 1 sonucu
#define HALL_POLARITY_MASK      0       // Senaryo 1 sonucu
#define HALL_POLARITY_INVERT    0       // Senaryo 1 sonucu

/* === Akım Koruma (ölçümle belirlenecek) === */
#define CURRENT_SOFT_LIMIT      450     // Senaryo 2 sonucu
#define CURRENT_HARD_LIMIT      700     // Senaryo 2 sonucu
#define HARD_LIMIT_STRIKES      3       // Senaryo 2 sonucu

/* === Duty ve Ramp (ölçümle belirlenecek) === */
#define DUTY_MIN_ACTIVE         8       // Senaryo 3 sonucu
#define DUTY_DEFAULT            70      // Senaryo 3 sonucu
#define DUTY_RAMP_UP_STEP       2       // Senaryo 3 sonucu
#define DUTY_RAMP_DOWN_STEP     4       // Senaryo 3 sonucu

/* === Analog Ölçekleme (ölçümle belirlenecek) === */
#define INA_GAIN_DEFAULT        20.0f   // INA181A1 varyantı, 20 V/V
#define VSENSE_DIVIDER_RATIO    0.04472f // R_top=47k, R_bot=2.2k
```

Her sabit yanında yorumla hangi senaryodan geldiğini belirtecek.

---

## Phase 0 Uygulama Sırası (Detay)

Her adım bağımsız commit'lenebilir:

1. **KN#4**: `stm32f4xx_hal_conf.h:55` TICK_INT_PRIORITY 0→15
2. **KN#5**: `protection.c:41-44` volatile ekle (faultLatched, softLimitActive, hardStrikes, faultReason)
3. **KN#6**: `cli.c:79` INT32_MIN guard ekle
4. **KN#11**: `main.c:78` nowUs hesabını 64-bit'e taşı veya wrap-around
5. **KN#9**: `board_io.c:352` EOC polling'e timeout sayacı ekle
6. **KN#8**: `cli.c` buf[8] → buf[16] veya decimals clamp
7. **KN#10**: `protection.c` bool filterInitialized flag
8. **KN#12**: `cli.c` gain help mesajını düzelt
9. **KN#13**: `bldc_commutation.c` CCER maskeli read-modify-write
10. **KN#7**: `protection.h` FaultState sil veya kullan
11. **N1**: `hall.h:62` yorum düzelt
12. **N2**: `hall.c` Hall_GetDriveState sil veya işaretle
13. **N4**: `cli.c:48` gereksiz extern sil
14. **KN#14**: `platformio.ini` build_unflags temizle
15. **KN#26**: `board_io.c` MOE assert
16. **KN#15-16**: `motor_config.h` clock yorumları düzelt (100→96, Arduino temizle)
17. **KN#17**: `board_io.c` ADC clock yorumları düzelt
18. **KN#18**: `board_io.h` SYSCLK yorumu
19. **KN#19**: `known_issues.md` outdated bölüm 4
20. **KN#21**: `protection.c:181` null-termination
21. **KN#20**: `board_io.c:134` Hall pin speed HIGH→LOW
22. **KN#23**: `board_io.c` RCC GPIOA tekrarlı enable
23. **N3**: `control_strategy.md` slew dead-time belgeleme

## Doğrulama

- Her Phase 0 adımından sonra `pio build` ile derleme kontrolü
- Phase 1 her senaryodan sonra `status`, `current`, `hall` CLI komutlarıyla değer doğrulama
- Phase 2 kararlar osiloskop + CLI verisi ile verilecek
