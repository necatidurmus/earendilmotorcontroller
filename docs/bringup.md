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
