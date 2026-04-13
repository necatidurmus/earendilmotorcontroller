# Earendil BLDC Motor Kontrolcüsü

STM32F411 Black Pill tabanlı, L6388 gate driver, hall sensörlü, senkron komplementer PWM BLDC motor firmware projesi.

---

## 1. Proje Özeti

Sensörlü 6-adım (6-step) trapezoidal komütasyon yapısı kullanan, STM32Cube HAL tabanlı, modüler ve belgelenmiş bir BLDC motor sürücü firmware'idir. Arduino bağımlılığı yoktur. Motor kontrol hot path yalnızca register seviyesinde çalışır.

---

## 2. Donanım Özeti

| Bileşen | Detay |
|---|---|
| MCU | STM32F411CEU6 (WeAct Black Pill V3) |
| Güç aşaması | 3x L6388ED013TR gate driver + 6x NMOS (part bilinmiyor) |
| Bootstrap | 1 µF kapasitörler + diyotlar (her faz için) |
| Gate direnci | 22 Ω |
| Akım ölçümü | INA181 + 0.5 mΩ shunt (INA181 suffix bilinmiyor) |
| Voltaj ölçümü | PA4, bölücü oranı şematikten hesaplanmış |
| Hall sensörler | Dahili pull-up |
| Besleme | Güç katı + 3.3V regülatör |

---

## 3. Pin Tablosu

| Pin | Fonksiyon | Yön | Notlar |
|---|---|---|---|
| PA8 | TIM1_CH1 — Yüksek taraf A (INH) | Çıkış | AF1 |
| PA9 | TIM1_CH2 — Yüksek taraf B (INH) | Çıkış | AF1 |
| PA10 | TIM1_CH3 — Yüksek taraf C (INH) | Çıkış | AF1 |
| PA7 | TIM1_CH1N — Düşük taraf A (INL) | Çıkış | AF1 |
| PB0 | TIM1_CH2N — Düşük taraf B (INL) | Çıkış | AF1 |
| PB1 | TIM1_CH3N — Düşük taraf C (INL) | Çıkış | AF1 |
| PB6 | Hall A | Giriş | Pull-up |
| PB7 | Hall B | Giriş | Pull-up |
| PB8 | Hall C | Giriş | Pull-up |
| PA0 | ISENSE (ADC1_IN0) | Analog giriş | INA181 çıkışı |
| PA4 | VSENSE (ADC1_IN4) | Analog giriş | Bölücü |
| PA2 | USART2_TX | Çıkış | CLI |
| PA3 | USART2_RX | Giriş | CLI |
| PC13 | LED | Çıkış | Aktif düşük |

> **Not:** PA9/PA10 hem TIM1_CH2/CH3 hem USART1_TX/RX olarak kullanılabilir. PWM için seçildiğinden CLI USART2'ye (PA2/PA3) taşındı.

---

## 4. Proje Klasör Yapısı

```
earendilmotorcontroller/
├── platformio.ini
├── README.md
├── docs/
│   ├── architecture.md      Genel mimari açıklaması (saat ağacı, ISR akışı)
│   ├── control_strategy.md  Senkron komplementer PWM teorisi ve CCER tablosu
│   ├── pinout.md            Detaylı pin açıklamaları ve çakışma notları
│   ├── bringup.md           İlk enerjilemeden motora doğrulama sırası
│   ├── modules.md           Her .c/.h dosyasının görevi ve API'si
│   ├── cli_reference.md     Tüm CLI komutlarının detaylı açıklaması
│   └── config_reference.md  motor_config.h sabitleri ve hesaplama rehberi
├── include/
│   ├── motor_config.h       Tüm pin ve parametre tanımları
│   ├── board_io.h           Donanım başlatma API'si
│   ├── bldc_commutation.h   Komütasyon API'si ve RunMode
│   ├── hall.h               Hall sensör işleme API'si
│   ├── protection.h         Koruma ve ADC API'si
│   ├── cli.h                CLI API'si
│   ├── stm32f4xx_hal_conf.h HAL konfigürasyonu
│   └── stm32f4xx_it.h       ISR bildirimleri
└── src/
    ├── main.c               Giriş noktası, ISR çağrısı, ana döngü
    ├── board_io.c           TIM1/TIM3/ADC/UART/GPIO başlatma
    ├── bldc_commutation.c   6-adım senkron komplementer komütasyon
    ├── hall.c               Hall okuma, majority vote, debounce
    ├── protection.c         EMA, soft/hard limit, fault latch
    ├── cli.c                UART2 CLI, komut dispatcher
    └── stm32f4xx_it.c       TIM3 ISR → MotorControl_Tick()
```

---

## 5. Her Dosya Ne İşe Yarıyor

| Dosya | Açıklama |
|---|---|
| `motor_config.h` | Tüm pin, timer, ADC, koruma sabitleri. Değiştirme noktası. |
| `board_io.c` | TIM1 komplementer PWM + deadtime, GPIO, ADC, UART başlatma. |
| `bldc_commutation.c` | 6 adım × 2 yön CCER lookup tablosu. ISR hot path. |
| `hall.c` | 7× majority vote, debounce, geçersiz hall hold. |
| `protection.c` | EMA filtreli akım, soft/hard limit, fault latch, slew. |
| `cli.c` | Non-blocking UART2 CLI, 15+ komut. |
| `main.c` | MotorControl_Tick() ve main loop. |
| `stm32f4xx_it.c` | TIM3_IRQHandler → MotorControl_Tick(). |

---

## 6. Kontrol Mimarisi

```
[TIM3 ISR @ 12.5 kHz]
     │
     ├─ Prot_SampleTick()     ← ADC örnekle (decimated)
     ├─ Prot_CheckHardLimit() ← hard overcurrent?
     ├─ Hall_ResolveState()   ← hall → state 0..5
     ├─ Prot_ApplySoftLimit() ← soft current limit
     ├─ Prot_SlewDuty()       ← rampa
     └─ Comm_ApplyStep()      ← TIM1 CCR + CCER yaz

[Ana Döngü]
     ├─ CLI_Service()         ← UART2 komut işleme
     └─ LED blink
```

---

## 7. Senkron Komplementer PWM

### Asenkron (önceki) yöntem
- Yüksek taraf: TIM1 PWM
- Düşük taraf: GPIO sabit HIGH
- Deadtime: yazılımda, bir ISR periyodu all-off

### Senkron Komplementer (mevcut) yöntem
- Yüksek taraf: TIM1_CHx — PWM sinyali
- Düşük taraf: TIM1_CHxN — komplementer (otomatik ters + donanım deadtime)
- Deadtime: BDTR.DTG = 50 → 500 ns MCU + ~300-400 ns L6388 dahili = ~800-900 ns toplam

### Bu projede nasıl uygulandı

Her komütasyon adımı için hangi CHx/CHxN çiftinin aktif olacağı CCER lookup tablosunda saklanır:

```
Adım 0: A↑ B↓ → CCER = CH1E | CH2NE
Adım 1: A↑ C↓ → CCER = CH1E | CH3NE
...
```

Komütasyon adımı başına yalnızca 4 register yazımı yapılır: CCR1/2/3 sıfırlama + aktif CCR + CCER.

### L6388 ile gerçekçi tablo

L6388 ayrı INH ve INL girişlerine sahiptir. TIM1_CHx → INH, TIM1_CHxN → INL. Bu pin routing textbook complementary PWM için idealdir. Shoot-through donanım tarafından önlenir.

---

## 8. Hall İşleme Mantığı

- Her ISR tick'inde 7 kez okunur, çoğunluk oyuyla gürültü filtrelenir
- `MIN_STATE_INTERVAL_US = 40 µs` debounce: çok hızlı değişimler reddedilir
- `INVALID_HALL_HOLD_US = 1500 µs`: geçersiz hall okunursa son geçerli durum tutulur
- Geçersiz durum süresi aşılırsa: çıkışlar kapalı
- `polarityMask` ile faz polaritesi, `stateOffset` ile adım kayması, `profile` ile 4 farklı hall/faz eşleştirmesi ayarlanabilir

---

## 9. Protection Mantığı

| Mekanizma | Açıklama |
|---|---|
| Ofset kalibrasyonu | Başlangıçta 128 örnekle sıfır akım ofsetini öğren |
| EMA filtre | α=0.20, yüksek frekanslı gürültüyü bastır |
| Soft limit | ADC delta > `CURRENT_SOFT_LIMIT` → duty azalt |
| Hard limit | ADC delta > `CURRENT_HARD_LIMIT` ardı ardına 3 kez → fault latch |
| Fault latch | Çıkışlar kapalı, mod STOPPED, 'clear' olmadan tekrar çalışmaz |
| Geçersiz hall | Çıkışlar hemen kapalı |
| Undervoltage | **TODO** — VSENSE ölçeği doğrulanmadan eklenmedi |
| Termal | **TODO** — NTC eklendikten sonra aktif edilecek |

---

## 10. Derleme Adımları

```bash
# PlatformIO kurulu değilse:
pip install platformio

# Derleme:
cd earendilmotorcontroller
pio run
```

---

## 11. Flash Etme Adımları

```bash
# ST-Link bağlı iken:
pio run --target upload

# Monitör (CLI):
pio device monitor --baud 115200
```

---

## 12. CLI Komutları

| Komut | Açıklama |
|---|---|
| `forward` / `f` | İleri yön |
| `backward` / `b` | Geri yön |
| `stop` / `s` | Durdur |
| `pwm <0..255>` | Duty değeri |
| `status` | Tam durum raporu |
| `hall` | Hall anlık görüntü |
| `current` | Akım anlık görüntü |
| `hinv <0\|1>` | Hall polarite tersle |
| `hmask <0..7>` | Hall XOR maskesi |
| `offset <-5..5>` | Durum kaydırma |
| `map <0..3>` | Hall profili seç |
| `limits <s> <h>` | Soft/hard ADC limitler |
| `gain <val>` | INA181 kazancı (görüntü için) |
| `zeroi` | Akım ofset kalibrasyonu |
| `clear` | Fault temizle |
| `help` | Yardım |

---

## 13. İlk Bench Test Sırası

1. **Güç vermeden önce:** Tüm bağlantıları kontrol et. Gate dirençleri doğru mu? Bootstrap kapasitörleri var mı?
2. **Düşük voltajla güç ver** (örn. 12V, varsa limitli güç kaynağı 0.5A limit)
3. **LED yanıp sönsün** — main loop çalışıyor
4. **CLI bağlan:** `pio device monitor --baud 115200`
5. **`status`** → hall değerlerini gör, fault yok olsun
6. **Motoru elle döndür** → `hall` komutuyla hall geçişlerini gör
7. **`pwm 10`** → düşük duty ayarla
8. **`forward`** → motor kıpırdamalı
9. Osiloskopla PA8/PA7 çiftini gör — komplementer dalga + deadtime
10. Duty yavaş artır: `pwm 20`, `pwm 30`, ...
11. `status` ile akım takip et
12. `stop` ile durdur

---

## 14. Bilinen Belirsizlikler

| Konu | Durum |
|---|---|
| MOSFET part number | Bilinmiyor — Vds, Rds(on), Qg kontrol edilmeli |
| INA181 suffix (kazanç) | Bilinmiyor — PCB üzerindeki markaj okunmalı |
| Deadtime yeterliliği | 500 ns MCU + ~400 ns L6388 = ~900 ns — bench'te osiloskopla doğrulanmalı |
| VSENSE bölücü oranı | Teorik hesap, gerçek R değerleri ölçülmeli |
| Hall profili | Profil 0 varsayılan — motora göre ayarlanmalı |
| Geri yön | Bench'te doğrulanmadı — yanlışsa `backward`'ı test et |

---

## 15. Gelecek Geliştirmeler

- [ ] DMA veya timer-triggered ADC (blocking ADC → ISR yükü azalt)
- [ ] TIM1 Break girişine OCP bağlantısı (donanım overcurrent trip)
- [ ] Undervoltage koruması (VSENSE doğrulandığında)
- [ ] Termal koruma (NTC eklendikten sonra)
- [ ] Hız geri beslemesi (hall geçiş süresi → RPM)
- [ ] Closed-loop PI hız kontrolü
- [ ] USB CDC CLI (UART2 yerine, PA9/PA10'u kurtarmak için)
