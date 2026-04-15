# Serial Monitor Test Rehberi

Bu dosya, seri monitorde hangi komutu yazacagini, hangi testleri yapacagini ve testte ne gormen gerektigini adim adim verir.

## 1) Seri monitor baglantisi

```bash
pio device monitor --baud 115200
```

Ilk acilista asagidakine benzer satirlar gorulmeli:

```text
Calibrating ISENSE...
Earendil BLDC Controller CLI
Commands:
```

Notlar:
- Komutlar buyuk/kucuk harf duyarsizdir.
- Enter basmasan bile komut ~120 ms sonra parse edilir.

## 2) Hemen calisan hizli test akisi

Asagiyi sirayla yaz:

```text
help
status
stop
pwm 0
zeroi
current
hall
pwm 8
forward
status
stop
backward
status
stop
```

## 3) Test listesi (komut + beklenen)

### Test-01 CLI canli mi?
Seri monitore yaz:

```text
help
```

Beklenen:
- `Earendil BLDC Controller CLI`
- `Commands:`
- `forward|f`, `backward|b`, `status`, `current` satirlari

### Test-02 Sistem durumu dogru mu?
Seri monitore yaz:

```text
status
```

Beklenen:
- `=== EARENDIL STATUS ===`
- `Mode: STOPPED` (baslangicta)
- `Fault: none` (normal durumda)
- `Hall raw=... corr=... map=... acc=... drvHall=... drvAct=...`
- `I raw=... filt=... off=... dlt=... estA=... V est=... raw=... uvStrikes=...`

### Test-03 STOPPED koruma kurali
Amaç: config komutlari calisirken motorun durmasi zorunlu mu?

Seri monitore yaz:

```text
pwm 10
forward
map 1
```

Beklenen:
- `map` komutu reddedilir
- Mesaj: `'map' only allowed in STOPPED mode (applied duty must be 0).`

Temizlemek icin:

```text
stop
pwm 0
```

### Test-04 Akim ofset kalibrasyonu
Seri monitore yaz:

```text
zeroi
current
```

Beklenen:
- `Calibrating ISENSE offset...`
- `Offset=<sayi> done`
- `current` cikisinda `off` dolu gelir
- Motor bos/stop iken `dlt` kucuk olur

### Test-05 Hall okunuyor mu?
Motoru ELLE yavas dondur. Sonra tekrar tekrar yaz:

```text
hall
```

Beklenen:
- `raw` ve `corr` 3 bit degisir (000/111'e takili kalmaz)
- Gecerli durumda `map` ve `acc` alanlari 0..5 araliginda gezer
- `drvAct` stop modunda genelde `OFF` olabilir

### Test-06 Dusuk duty ileri donus
Seri monitore yaz:

```text
stop
pwm 0
pwm 8
forward
status
current
```

Beklenen:
- `Mode: FORWARD`
- `Duty cmd=8 applied=...` (slew nedeniyle ilk an cmd'den kucuk olabilir)
- `Comm state=0..5` veya o anki sektor
- `Fault: none`

### Test-07 Geri yon
Seri monitore yaz:

```text
stop
pwm 8
backward
status
```

Beklenen:
- `Mode: BACKWARD`
- Motor yonu ileriye gore ters
- Fault yoksa `Fault: none`

### Test-08 Fault tetikleme ve clear
Amaç: hard overcurrent latch ve `clear` davranisi.

Seri monitore yaz:

```text
stop
pwm 0
limits 1 2
pwm 20
forward
status
```

Beklenen:
- Kisa surede fault latch olur
- `status` icinde `Fault: hard overcurrent`
- Motor durur (`Mode: STOPPED` veya applied duty 0)

Sonra fault temizleme:

```text
clear
status
```

Beklenen:
- `Fault cleared. Re-arm with pwm + mode command.`
- Sonraki `status`: `Fault: none`

Not: Bu testten sonra normal limitleri geri yukle.

```text
stop
pwm 0
limits 450 700
```

### Test-09 Undervoltage ayari komut testi
Bu test sadece CLI davranisini dogrular (gercek UV fault icin ayarlanabilir PSU gerekir).

Seri monitore yaz:

```text
stop
pwm 0
uven 1
uv 9000 500 8
status
```

Beklenen:
- `UV enable=1`
- `UV cfg set: lim=9000 hyst=500 strikes=8`
- `status` satirinda `UV en=1 lim_mV=9000 hyst_mV=500 strikes=8`

## 4) Hata durumda gorecegin tipik metinler

- Bilinmeyen komut: `Unknown. Type 'help'.`
- Fault varken run komutu: `FAULT active. Use 'clear' first.`
- `clear` icin duty sifir degilse: `Set pwm 0 before clear.`
- `clear` icin motor stop degilse: `Stop motor first before clear.`

## 5) Kisa pass/fail checklist

- [ ] `help` calisiyor
- [ ] `status` dogru blok basiyor
- [ ] `zeroi` offset uretiyor
- [ ] Hall elde cevirince degisiyor
- [ ] Dusuk duty `forward` donuyor
- [ ] `backward` ters yone donuyor
- [ ] Fault tetiklenip `clear` ile temizleniyor
- [ ] UV ayarlari `status` icinde gorunuyor
