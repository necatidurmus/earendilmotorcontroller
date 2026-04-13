# Kontrol Stratejisi: Senkron Komplementer PWM

## Geleneksel Asenkron Sürüş

```
Yüksek taraf: PWM sinyali (duty % açık)
Düşük taraf:  GPIO sabit HIGH (her zaman açık)
3. faz:       Her ikisi kapalı (float)
```

Sorunlar:
- Shoot-through riski: yazılım deadtime güvenilmez
- Geri dönüş akımı body diodundan geçer (verimsiz)
- Yazılımda `all-off` bekleme: kaba ve konfigürasyon bağımlı

## Senkron Komplementer Sürüş

```
Yüksek taraf: TIM1_CHx → PWM sinyali (CCR duty)
Düşük taraf:  TIM1_CHxN → komplementer (otomatik ters + donanım deadtime)
3. faz:       CCER = 0 → her ikisi devre dışı → idle state = LOW
```

Avantajlar:
- Donanım deadtime: BDTR.DTG garantili, yazılımsız
- Shoot-through riski: timer donanımı tarafından elimine edilir
- Senkron redresyon: düşük taraf MOSFET geri akımı iletir (body diod yerine)
- Temiz geçişler: CCER atomik değer → anlık glitch minimum

## Deadtime Hesabı

```
TIM1 tick: 10.4 ns (96 MHz)
DEADTIME_COUNTS = 50
MCU deadtime = 50 × 10.4 ns ≈ 521 ns

L6388 dahili propagasyon gecikmesi: ~300-400 ns (datasheet)
Toplam efektif deadtime ≈ 820-920 ns

[AYAR]: Bootstrap kapasitör şarj/deşarj süresi göz önünde bulundurularak
        bench'te osiloskopla doğrulanmalı. Gerekirse DEADTIME_COUNTS artırılabilir.
```

## 6-Adım Komütasyon Tablosu

### İleri Yön

| Hall (CBA) | Adım | Yük. Taraf | Düş. Taraf | CCER |
|---|---|---|---|---|
| 001 | 0 | A (CH1) | B (CH2N) | 0x041 |
| 011 | 1 | A (CH1) | C (CH3N) | 0x401 |
| 010 | 2 | B (CH2) | C (CH3N) | 0x410 |
| 110 | 3 | B (CH2) | A (CH1N) | 0x014 |
| 100 | 4 | C (CH3) | A (CH1N) | 0x104 |
| 101 | 5 | C (CH3) | B (CH2N) | 0x140 |

### Geri Yön

Yüksek ve düşük taraf yer değiştirir (faz terslemesi):

| Adım | Yük. Taraf | Düş. Taraf | CCER |
|---|---|---|---|
| 0 | B (CH2) | A (CH1N) | 0x014 |
| 1 | C (CH3) | A (CH1N) | 0x104 |
| 2 | C (CH3) | B (CH2N) | 0x140 |
| 3 | A (CH1) | B (CH2N) | 0x041 |
| 4 | A (CH1) | C (CH3N) | 0x401 |
| 5 | B (CH2) | C (CH3N) | 0x410 |

## L6388 ile Donanım Uyumu

L6388 ayrı INH (yüksek taraf) ve INL (düşük taraf) girişleri sunar.

```
TIM1_CH1  (PA8)  → L6388_A.INH → MOSFET_AH gate
TIM1_CH1N (PA7)  → L6388_A.INL → MOSFET_AL gate
TIM1_CH2  (PA9)  → L6388_B.INH → MOSFET_BH gate
TIM1_CH2N (PB0)  → L6388_B.INL → MOSFET_BL gate
TIM1_CH3  (PA10) → L6388_C.INH → MOSFET_CH gate
TIM1_CH3N (PB1)  → L6388_C.INL → MOSFET_CL gate
```

Bu pin routing **textbook complementary PWM için idealdir**.

### L6388 Dahili Bootstrap Mantığı

L6388 INH=0, INL=0 durumunda her iki MOSFET'i kapalı tutar.
INH=1 → yüksek taraf açılır.
INL=1 → düşük taraf açılır.
Eş zamanlı INH=INL=1 durumu L6388 tarafından dahili olarak engellenemez —
bu nedenle TIM1 deadtime kritiktir.

## Pasif Faz Davranışı

Aktif olmayan faz için CCER'da hem CHxE hem CHxNE = 0.
`OSSR=1` (TIM_OSSR_ENABLE) ayarı nedeniyle:
- Devre dışı çıkış → idle state → pin LOW
- L6388 INH=0, INL=0 → her iki MOSFET kapalı

Bu, pasif fazın float değil, her iki MOSFET'in kapalı olduğu anlamına gelir.
Geri EMF body diodundan değil, devre açıktan (yüksek empedans) iletilir.
Bu sensörlü komütasyon için doğru ve güvenli davranıştır.

## State Geçiş Slew (All-Off Dead-Time)

Hall sensör değişimi algılandığında, mevcut implementasyon bir tam ISR
periyodu (~80 us) boyunca tüm çıkışları kapatır (CCR=0, CCER=0), sonra
yeni komütasyon adımını uygular.

```
Hall değişim algılandı → Comm_AllOff() → 1 ISR tick bekleme → Yeni step
```

Bu yaklaşım **güvenli** ancak **kaba**dır:
- Bir tam periyot boyunca motor enerjisiz kalır → tork ripple
- 80 us boyunca body diod üzerinden akım akar → verim kaybı
- Yüksek duty'de daha belirgin ses/titreşim

Güvenlik avantajı: software-level shoot-through koruması sağlar
(donanım deadtime'ın üzerine ek güvenlik katmanı).

İleride rafine edilebilir: CCR'ları sıfırla ama CCER'ı hemen yeni adıma
ayarla (donanım deadtime zaten shoot-through'yu önler), veya half-step
geçişleri uygula.
