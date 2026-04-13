# CLI Komut Referansı

UART2 üzerinden 115200 baud, PA2(TX)/PA3(RX). Herhangi bir seri terminal ile bağlanılır.

```bash
pio device monitor --baud 115200
```

Komutlar büyük/küçük harf duyarsızdır. Satır sonu `\r`, `\n` veya `\r\n` kabul edilir.
Satır sonu gelmezse `CLI_IDLE_PARSE_MS = 120 ms` sonra otomatik işlenir.

---

## Motor Kontrol Komutları

### `forward` veya `f`
Motoru ileri yönde başlatır.

- Fault aktifse çalışmaz: `FAULT active. Use 'clear' first.` mesajı verir.
- Duty değeri `pwm` komutuyla ayrıca ayarlanmalıdır.

### `backward` veya `b`
Motoru geri yönde başlatır.

- Geri yön: komütasyon tablosunda yüksek ve düşük taraf çifti yer değiştirir.
- Aynı fault koruması geçerlidir.

### `stop` veya `s`
Motoru durdurur.

- `g_runMode = RUN_STOPPED` yazar.
- ISR bir sonraki tick'te `Comm_AllOff()` çağırır.
- Fault temizlemez.

### `pwm <0..255>`
Komut duty değerini ayarlar.

```
pwm 50
```

- `0`: motor dönmez (stop gibi davranır ama mod değişmez)
- `255`: maksimum duty (`PWM_PERIOD_COUNTS = 3199` → ~100%)
- Gerçek uygulanan duty soft limit ve slew'den geçer: `g_appliedDuty` farklı olabilir
- `status` komutuyla `Duty cmd=` ve `applied=` karşılaştırılabilir

---

## Durum Sorgu Komutları

### `status`
Tam sistem durumu raporu.

Çıktı:
```
=== EARENDIL STATUS ===
Mode: FORWARD
Duty cmd=50 applied=48
Hall prof=0 mask=0 offset=0
Limits soft=450 hard=700
UV en=1 lim_mV=9000 hyst_mV=500 strikes=8
Fault: none
ISR ticks: 125000
Hall raw=010 corr=010 map=2 acc=2 drv=2
I raw=2048 filt=2049 off=2041 dlt=8 estA=0.13 V est=24.31 raw=1820 uvStrikes=0
Comm state=2 pwm=627
=======================
```

| Alan | Açıklama |
|---|---|
| Mode | Mevcut çalışma modu |
| Duty cmd/applied | Komut ve gerçekleşen duty |
| Hall prof/mask/offset | Hall konfigürasyonu |
| Limits soft/hard | ADC delta koruma eşikleri |
| UV en/lim/hyst/strikes | Undervoltage koruma ayarları |
| Fault | Fault durumu ve nedeni |
| ISR ticks | Toplam ISR çalışma sayısı |
| Hall raw/corr/map/acc/drv | Hall işleme zinciri anlık görüntüsü |
| I raw/filt/off/dlt/estA | Akım ölçüm zinciri |
| V est/raw/uvStrikes | VSENSE tahmini voltaj, ham ADC, UV strike sayısı |
| Comm state/pwm | Aktif komütasyon durumu ve timer CCR değeri |

### `hall`
Yalnızca hall snapshot.

```
Hall raw=010 corr=010 map=2 acc=2 drv=2
```

| Alan | Açıklama |
|---|---|
| `raw` | GPIO'dan okunan ham 3-bit hall (CBA sırası) |
| `corr` | polarityMask XOR sonrası |
| `map` | Profil tablosundan eşleştirilmiş durum (0..5 veya INV) |
| `acc` | Debounce sonrası kabul edilen (0..5 veya INV) |
| `drv` | Yön ve offset uygulaması sonrası sürüş durumu (0..5 veya OFF) |

Motoru elle döndürürken `drv` alanının 0→1→2→3→4→5→0 veya tersine dönmesi gerekir.

### `current`
Yalnızca akım snapshot.

```
I raw=2048 filt=2049 off=2041 dlt=8 estA=0.13 soft=off strikes=0 uv=0 | V est=24.31 raw=1820
```

| Alan | Açıklama |
|---|---|
| `raw` | Son ham ADC okuması (0..4095) |
| `filt` | EMA filtrelenmiş değer |
| `off` | Kalibrasyon ofseti (`zeroi` ile ayarlanır) |
| `dlt` | delta = filtered - offset (koruma bu değere bakar) |
| `estA` | Tahmini amper — INA181A1 (gain=20 V/V) ile hesaplanır |
| `soft` | Soft limit aktif mi (ACT / off) |
| `strikes` | Ardışık hard limit aşım sayısı (0'dan 3'e ulaşınca fault) |
| `uv` | Ardışık undervoltage strike sayısı |
| `V est/raw` | VSENSE tahmini voltaj ve ham ADC |

---

## Hall Konfigürasyon Komutları

### `hmask <0..7>`
Hall polarite XOR maskesi.

```
hmask 7
```

- `0`: normal (XOR yok)
- `7`: tüm halleri tersle (= `hinv 1`)
- `1..6`: seçili bitleri tersle

Motorun hall sensörleri ters bağlıysa `7` kullanılır.

### `hinv <0|1>`
Kısayol: `hmask 0` veya `hmask 7`.

```
hinv 1
hinv 0
```

### `offset <-5..5>`
Komütasyon durum kaydırması.

```
offset 1
offset -2
```

Motor doğru dönüyor ama titriyor veya tam verimde değilse küçük offset (±1, ±2) denenir.

### `map <0..3>`
Hall → durum eşleştirme profilini değiştir.

```
map 0
map 1
```

Motor hiç dönmüyorsa (hall geçerliyse) farklı profiller denenir.
Profil 0 mevcut motor kablolamasıyla test edilmiştir.

---

## Akım/Koruma Komutları

### `limits <soft> <hard>`
ADC delta koruma eşiklerini çalışma zamanında değiştir.

```
limits 300 600
```

- `soft`: yumuşak limit — aşıldığında duty azaltılır
- `hard`: sert limit — 3 ardışık aşımda fault latch
- Koşul: `1 ≤ soft < hard ≤ 4095`
- Kalibrasyonsuz başlarken yüksek tutulur, yük testi sonrası indirilir

### `gain <1..1000>`
INA181 kazanç değerini ayarla (yalnızca `estA` görüntüsü etkiler).

```
gain 50
gain 100
```

PCB üzerindeki INA181 suffix'i okunduğunda:
- A1 → `gain 20`
- A2 → `gain 50`
- A3 → `gain 100`
- A4 → `gain 200`

**Koruma kararlarını etkilemez** — yalnızca `estA` hesaplamasını etkiler.

### `uv <limit_mV> <hyst_mV> <strikes>`
Undervoltage eşiğini runtime günceller.

```
uv 9000 500 8
```

- `limit_mV`: alt voltaj eşiği (1000..60000)
- `hyst_mV`: bırakma histerezisi (0..5000)
- `strikes`: latch için ardışık düşük voltaj sayısı (1..50)

### `uven <0|1>`
Undervoltage fault kontrolünü aç/kapatır.

```
uven 1
uven 0
```

### `zeroi`
Akım sensörü sıfır ofset kalibrasyonu.

```
zeroi
```

- Motor durdurulmuş ve çıkışlar kapalıyken çağrılmalıdır
- 128 ADC örneği alır, ortalamasını `currentOffset` olarak saklar
- Sonraki tüm delta hesaplamalarında bu ofseti çıkarır
- Her güç açılışında otomatik olarak `main.c`'de bir kez çalışır

### `clear`
Fault latch'i temizle.

```
clear
```

- `hard overcurrent`, `invalid hall timeout` gibi latch'li fault'ları temizler
- Temizledikten sonra motor `forward` veya `backward` ile tekrar başlatılabilir
- Fault nedeni neydi: `status` ile önceden görülmeli

---

## Yardım

### `help` veya `?`
Tüm komutların kısa listesini yazdır.

---

## Örnek Kullanım Senaryosu

```
# 1. Başlangıç kontrolü
status

# 2. Akım sensörü kalibrasyon (motorlar kapalıyken)
zeroi

# 3. Düşük duty ile başla
pwm 10

# 4. Motoru başlat
forward

# 5. Hall akışını izle
hall

# 6. Akım durumunu izle
current

# 7. Duty artır
pwm 25
pwm 50

# 8. Durdur
stop

# 9. Geri yön dene
backward
pwm 30

# 10. Fault durumunda temizle
clear
```
