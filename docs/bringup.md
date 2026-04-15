# İlk Enerjilemeden Motora Doğrulama Sırası

## Güvenli Enerjilemeden Önce Kontrol Listesi

- [ ] Kart polaritesi doğru mu?
- [ ] Bootstrap kapasitörleri yerinde mi?
- [ ] Gate dirençleri (22Ω) monteli mi?
- [ ] MOSFET'ler ısı yalıtımıyla mı monte edilmiş?
- [ ] Shunt (0.5 mΩ) bağlantısı doğru mu?
- [ ] INA181 bağlantısı doğru mu?
- [ ] Hall sensörler pull-up mevcut mu?
- [ ] ST-Link bağlı mı?

## Adım 1: MCU Doğrulama (Motor Yok)

```bash
pio run
pio run --target upload
pio device monitor --baud 115200
```

Beklenen çıktı:
```
Calibrating ISENSE...
========================================
 Earendil BLDC Motor Controller
 ...
========================================

Earendil BLDC Controller CLI
Commands:
  ...
```

LED yanıp sönüyorsa main loop çalışıyor.

## Adım 2: Hall Doğrulama (Motor Bağlı, Güç Yok)

Motoru elle döndürürken:
```
hall
```
Hall A/B/C değerlerinin 001..110 arasında değiştiğini gör (000 ve 111 geçersiz).

Hall 6 farklı durum dönüyorsa sensörler doğru bağlı.

Eğer sabit 111 veya 000 geliyorsa: pull-up eksik veya kablo sorunu.

## Adım 3: PWM Çıkış Doğrulaması (Motor Yok, Güç Yok)

**Osiloskop:** PA8 (CH1, yüksek taraf A) ve PA7 (CH1N, düşük taraf A)

```
pwm 50
forward
```

Beklenen:
- PA8: ~15 kHz PWM, ~%50 duty (tam olarak: 50/255 × 30 kHz periyodu)
- PA7: PA8'in tersi + ~500-900 ns deadtime
- İkisi aynı anda HIGH olmamalı (shoot-through yok)

```
stop
```

## Adım 4: Gate Sürücü Doğrulama (Düşük Voltaj, Motor Yok)

**Limitli güç kaynağı: 12V, 0.5A limit**

```
pwm 10
forward
```

- MOSFET gate'lerini ölç (AH gate = PA8 × kazanç, AL gate = PA7 × kazanç)
- Deadtime osiloskopla gör
- Güç kaynağı akım göstergesi çok küçük olmalı (motor yok → sadece gate şarj akımı)

## Adım 5: İlk Motor Testi

**Limitli güç kaynağı: düşük voltaj, 1A limit**

```
zeroi          ← ofset kalibrasyonu (outputs off iken)
pwm 8          ← çok düşük duty
forward        ← başlat
status         ← akım ve hall durumuna bak
```

Motor kıpırdıyorsa: başarılı.

Adım adım artır: `pwm 15`, `pwm 25`, ...

## Adım 6: Protection Kalibrasyonu

`current` komutuyla ADC delta değerini izle.
Yük altında maximum kabul edilebilir akıma karşılık gelen delta'yı not et.
`motor_config.h` içinde `CURRENT_SOFT_LIMIT` ve `CURRENT_HARD_LIMIT` değerlerini ayarla.

## Osiloskop Kontrol Noktaları

| Nokta | Beklenen |
|---|---|
| PA8 ve PA7 | Komplementer, deadtime görünür |
| PA9 ve PB0 | Aynı, faz B için |
| PA10 ve PB1 | Aynı, faz C için |
| Hall değişimi → CCER değişimi | <1 ISR periyodu gecikme |
| Fault durumu | Tüm pinler LOW |

## Bilinen Riskler

1. **Yanlış hall profili:** Motor titreyebilir veya dönmeyebilir. `map 1`, `map 2`, `map 3` dene.
2. **Yanlış hall polaritesi:** `hinv 1` ile tüm halleri tersle.
3. **Yanlış yön:** `forward` yanlış dönüyorsa iki motor kablosunu yer değiştir.
4. **Deadtime yetersiz:** Gate'lerde eş zamanlı HIGH görünüyorsa `DEADTIME_COUNTS` artır.

---

## Ölçüm ve Kalibrasyon Rehberi

Aşağıdaki tüm ölçüm adımları `motor_config.h` sabitlerini belirlemek için yapılır. Her ölçüm sonucu ilgili sabite yazılmalıdır.

### 1. Akım Ölçüm Kalibrasyonu

**Gerekli araçlar:** Dijital multimetre, güç kaynağı (akım göstergeli), motor

**Adımlar:**

1. Motor bağlı, güç yok, `zeroi` komutuyla ofset kalibre edilir
2. `status` komutuyla `I off=` değeri not edilir → bu ADC ofset değeri
3. Boşta düşük duty (`pwm 10`) ile motor döndürülür
4. `status` ile `I dlt=` (delta) değeri not edilir → bu boşta akım ADC delta
5. Güç kaynağı akım göstergesi ile gerçek akım karşılaştırılır
6. `INA_GAIN_DEFAULT` doğrulanır: `dlt * 3.3 / 4095 / gain / 0.0005 ≈ gerçek akım (A)`
7. Yüklü motor ile max kabul edilebilir akımda `dlt` not edilir
8. `CURRENT_SOFT_LIMIT` = boşta dlt × 1.5 (güvenlik marjı)
9. `CURRENT_HARD_LIMIT` = max yük dlt × 0.9 (koruma eşiği)

**Ölçüm tablosu:**

| Durum | Güç Kaynağı Akım (A) | `I dlt=` | Hesaplanan Akım (A) | Notlar |
|---|---|---|---|---|
| Boşta pwm 10 | | | | |
| Boşta pwm 50 | | | | |
| Yüklü pwm 50 | | | | |
| Max yük | | | | |

**Config sabitleri:**
- `CURRENT_SOFT_LIMIT` = ___ (ADC delta)
- `CURRENT_HARD_LIMIT` = ___ (ADC delta)
- `HARD_LIMIT_STRIKES` = ___ (3-5 arası, gürültüye göre)
- `INA_GAIN_DEFAULT` = ___ (ölçülen kazanç)

### 2. Voltaj Ölçüm Kalibrasyonu

**Gerekli araçlar:** Dijital multimetre, ayarlanabilir güç kaynağı

**Adımlar:**

1. Güç kaynağından bilinen voltaj uygula (12V, 24V, 36V)
2. `status` komutuyla `V raw=` ve `V est=` değerlerini not et
3. Multimetre ile gerçek voltajı ölç
4. `VSENSE_DIVIDER_RATIO` doğrulanır: `raw * 3.3 / 4095 / gerçek_voltaj ≈ bölücü oranı`
5. Gerekirse `VSENSE_DIVIDER_RATIO` güncellenir

**Ölçüm tablosu:**

| Uygulanan Voltaj (V) | Multimetre (V) | `V raw=` | `V est=` | Hesaplanan Oran |
|---|---|---|---|---|
| 12.0 | | | | |
| 24.0 | | | | |
| 36.0 | | | | |

**Config sabitleri:**
- `VSENSE_DIVIDER_RATIO` = ___ (ölçülen oran)
- `UNDERVOLTAGE_LIMIT_MV` = ___ (min çalışma voltajı, mV)
- `UNDERVOLTAGE_HYSTERESIS_MV` = ___ (geri dönüş histerizi, mV)

### 3. Hall Sensör Kalibrasyonu

**Gerekli araçlar:** Motor, seri terminal

**Adımlar:**

1. Motor bağlı, güç yok, elle döndür
2. `hall` komutuyla `raw` değerlerini izle (001→011→010→110→100→101→001 döngüsü olmalı)
3. Döngü doğru sıralamada değilse `map 0`, `map 1`, `map 2`, `map 3` dene
4. Yanlış yön varsa `hinv 1` dene
5. Doğru profil bulunduğunda düşük duty ile motor döndür
6. `status` ile `Comm state=` değerinin sıralı (0→1→2→3→4→5) olduğunu doğrula
7. Titreşim varsa `offset +1` veya `offset -1` dene

**Ölçüm tablosu:**

| Hall Profili | Ham Döngü | Doğru mu? | Yön | Notlar |
|---|---|---|---|---|
| map 0 | | | | |
| map 1 | | | | |
| map 2 | | | | |
| map 3 | | | | |

**Config sabitleri:**
- `HALL_TO_STATE_PROFILES` doğru profil indeksi = ___
- `HALL_POLARITY_MASK` = ___ (0 veya 7)
- State offset = ___ (-5..+5)

### 4. Deadtime Kalibrasyonu

**Gerekli araçlar:** Osiloskop (2 kanal), motor yok

**Adımlar:**

1. `pwm 128` ve `forward` ile çalıştır
2. Osiloskop kanal 1: PA8 (yüksek taraf A), kanal 2: PA7 (düşük taraf A)
3. İki sinyalin aynı anda HIGH olmadığını doğrula
4. Deadtime süresini ölç (düşen kenardan yükselen kenara)
5. Ölçülen deadtime < 500 ns ise `DEADTIME_COUNTS` artır
6. Ölçülen deadtime > 1500 ns ise verimlilik kaybı var, düşür

**Ölçüm tablosu:**

| DEADTIME_COUNTS | Ölçülen Deadtime (ns) | Shoot-through var mı? | Notlar |
|---|---|---|---|
| 50 (varsayılan) | | | |
| 40 | | | |
| 60 | | | |

**Config sabitleri:**
- `DEADTIME_COUNTS` = ___ (güvenli minimum değer)

### 5. Duty ve Slew Kalibrasyonu

**Gerekli araçlar:** Motor, güç kaynağı, seri terminal

**Adımlar:**

1. Motor boşta, düşük duty ile başlat
2. `pwm 0` → `pwm 255` komutunu hızlıca ver
3. `status` ile `applied=` değerinin kademeli arttığını gözle
4. Motor ani duty değişiminde titriyorsa `DUTY_SLEW_PER_TICK` düşür
5. Motor çok yavaş tepki veriyorsa `DUTY_SLEW_PER_TICK` artır
6. Motorun güvenle kalktığı minimum duty'yi bul: `pwm 1`, `pwm 2`, ... adım adım artır

**Ölçüm tablosu:**

| Duty Komutu | Motor Tepkisi | Akım (A) | Notlar |
|---|---|---|---|
| pwm 1 | | | |
| pwm 5 | | | |
| pwm 10 | | | |
| pwm 25 | | | |
| pwm 50 | | | |

**Config sabitleri:**
- `DUTY_MIN_ACTIVE` = ___ (motorun kalktığı minimum duty)
- `DUTY_SLEW_PER_TICK` = ___ (tick başına max duty değişim)

### 6. Under-Voltage Koruma Kalibrasyonu

**Gerekli araçlar:** Ayarlanabilir güç kaynağı, motor

**Adımlar:**

1. Motor çalışır durumda, voltajı yavaşça düşür
2. Motorun düzgün çalışmayı bıraktığı voltajı not et
3. `UNDERVOLTAGE_LIMIT_MV` = bu voltajın %10 altı (güvenlik marjı)
4. Voltajı tekrar yükselt, motorun normale döndüğü voltajı not et
5. `UNDERVOLTAGE_HYSTERESIS_MV` = limit ile geri dönüş voltajı farkı

**Ölçüm tablosu:**

| Voltaj (V) | Motor Durumu | `uvStrikes=` | Notlar |
|---|---|---|---|
| 24.0 | | | |
| 20.0 | | | |
| 18.0 | | | |
| 15.0 | | | |
| 12.0 | | | |

**Config sabitleri:**
- `UNDERVOLTAGE_LIMIT_MV` = ___ (mV)
- `UNDERVOLTAGE_HYSTERESIS_MV` = ___ (mV)
- `UNDERVOLTAGE_STRIKES` = ___ (3-10 arası)

### 7. EMA Filtre Kalibrasyonu

**Gerekli araçlar:** Motor, seri terminal

**Adımlar:**

1. Motor boşta dönerken `status` ile `I raw=` ve `I filt=` değerlerini izle
2. `raw` değeri çok dalgalanıyorsa `CURRENT_FILTER_ALPHA` düşür (daha yumuşak)
3. Filtre çok yavaş tepki veriyorsa `CURRENT_FILTER_ALPHA` artır (daha hızlı)
4. İyi filtre: `filt` değeri `raw` ortalamasını takip eder ama ani sıçramaları yumuşatır

**Config sabitleri:**
- `CURRENT_FILTER_ALPHA` = ___ (0.05-0.30 arası, düşük = daha yumuşak)

---

## Kalibrasyon Sonrası Kontrol Listesi

Tüm ölçüm tamamlandıktan sonra:

- [ ] `CURRENT_SOFT_LIMIT` ayarlandı mı?
- [ ] `CURRENT_HARD_LIMIT` ayarlandı mı?
- [ ] `VSENSE_DIVIDER_RATIO` doğrulandı mı?
- [ ] Doğru hall profili seçildi mi?
- [ ] Deadtime osiloskopla doğrulandı mı?
- [ ] `DUTY_MIN_ACTIVE` belirlendi mi?
- [ ] Under-voltage eşiği test edildi mi?
- [ ] Tüm `motor_config.h` değişiklikleri derlendi ve yüklendi mi?
- [ ] Son test: tam hız, tam yük, 5 dakika stabil çalışma?
