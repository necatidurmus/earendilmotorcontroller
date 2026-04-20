# Agent Kuralları

## İletişim — Kredi Tasarrufu

- **Kodlama/debug modu:** 5-6 kelimeyi geçme. Sadece anahtar kelimeler.
- **Arge/planlama/hata tartışma modu:** Kısa ve öz, ama anlaşılır.
- **Gereksiz açıklama, özet, giriş/sonuş cümleleri YASAK.**
- **Emoji kullanma.**
- **Kod yazarken yorum ekle:** Kısa, anahtar kelimelerden oluşan. Uzun cümle yok.
- **Cevap sonrası açıklama yapma.** Kodu yaz, dur.
- **Uzun çıktıları kısalt.** Kullanıcı isterse detay ver.

## Örnek

Kötü: "Bu fonksiyonun sorunu, değişkenin null kontrolü yapılmadan kullanılması. Şimdi düzeltiyorum..."
İyi: "Null check eksik. Düzeltiliyor."

Kötü: "İşte değişiklikler: 1) x değişkeni eklendi, 2) y fonksiyonu güncellendi, 3) testler düzeltildi. Umarım yardımcı olur!"
İyi: "x eklendi, y güncellendi, test düzeltildi."

---

## Build / Flash Komutları

- `pio run` — derle
- `pio run -t upload` — ST-Link ile flaşla
- `pio device monitor` — seri monitör
- `pio device monitor -b 115200` — USB seri monitör

Not: Upload protocol = ST-Link (platformio.ini). LSP hataları (Arduino.h not found) IDE sorunu, gerçek hata değil.

---

## Proje Yapısı

```
src/main.cpp              — tüm firmware (2213 satır, tek dosya)
tools/wasd_controller.py  — Python host (WASD UI, legacy)
docs/README.md            — proje dokümantasyonu
docs/ARCHITECTURE.md      — teknik mimari
docs/ROADMAP.md           — 7 fazlı yol haritası
docs/ISSUES.md            — doğrulanmış sorunlar
platformio.ini            — build config (STM32 F411CE, Arduino)
AGENTS.md                 — bu dosya
```

---

## Ana Mimari Noktaları

- Tek motor sürücü modülü — STM32 Black Pill F411CE
- 6-step BLDC komütasyon — Hall sensör geri bildirimli
- 3 çalışma modu: Normal (CLI), Python (WASD), Settings (temiz monitör)
- Motor tick: 60µs (16.6kHz), motorControlTick() ana döngüde
- UART: 115200 baud, PA2(TX) PA3(RX)
- EEPROM: Hall map, config, mod kalıcılığı
- Tek dosya firmware: tüm motor mantığı main.cpp içinde

---

## Motor Durum Makinesi

```
Stopped → (f/b) → Kick → (kickMs) → Running
Running → (yön değişimi) → NeutralWait → Kick → Running
Any → (stop/watchdog) → Stopped
Any → (hall fault) → Fault
```

Fazlar: Stopped(0), Kick(1), Running(2), NeutralWait(3), Fault(4)

---

## Kontrol Akışı (loop)

```
1. runMotorControlScheduler()    — 60µs motor tick
2. uartDrainToRing()             — seri veri topla
3. processRxRingToLines()        — satır parse et
4. processQueuedCommands()       — komutları işle
5. updateServiceTask()           — scan/test/identify
6. sendTelemetry()               — telemetri gönder
7. checkCommandWatchdog()        — failsafe (Normal/Settings)
```

---

## UART Protokolü

### Mevcut (Legacy WASD)

w=ileri, s=geri, x=dur, d=PWM+10, a=PWM-10

### Hedef (f/b/s)

f=ileri varsayılan PWM, f<duty>=ileri duty(0-255), b=geri, b<duty>=geri duty, s=dur

Lease: Her hareket komutu zaman damgasını yeniler. 800ms komut gelmezse motor durur.

### Telemetri Formatı

```
RPM:0,D:0,DIR:F,PH:2,PWM:150,PDIR:1,H:3
```

RPM=devir, D=mevcut duty, DIR=F/R, PH=faz(0-4), PWM=hedef duty, PDIR=yön(1/-1/0), H=ham Hall

---

## Kritik Bilinen Sorunlar

| Sorun | Öncelik | Çözüm |
|-------|---------|-------|
| Kuyruk 1 komut/işlem | Kritik | if→while |
| Watchdog Python'da kapalı | Yüksek | checkCommandWatchdog() ekle |
| Donanımsal watchdog yok | Yüksek | IWDG ekle (Faz 4) |
| Dead-time yok | Yüksek | Test et, delayMicroseconds ekle |
| Varsayılan PWM 60 | Orta | EEPROM'dan ayarlanabilir yap |
| Telemetri PWM karışıklığı | Orta | PWM_SET/PWM_ACT kullan |
| saveall mod kaydetmiyor | Orta | saveModeToStorage() ekle |
| Identify step 1ms | Orta | 50-100ms'ye çıkar |

Detay: docs/ISSUES.md

---

## Geliştirme Workflow'u

Git tag'leri her faz bitiminde:
- v0.1.0 — Dokümantasyon baseline
- v0.2.0 — Phase 0 (bug düzeltmeleri)
- v0.3.0 — Phase 1 (f/b/s protokolü)

Faz sırası: Stabilize → f/b/s → FTDI test → Python → Güvenlik → Hub → 4 motor

---

## Güvenlik Uyarıları

- Motor çalışıyor! Test sırasında fiziksel acil durdurma hazır olmalı
- Dead-time yok — MOSFET hasarı riski, sınırlı akım kaynağı kullan
- Python modunda watchdog yok — host çöküşünde motor çalışmaya devam eder
- Donanımsal watchdog yok — firmware takılırsa motor çalışmaya devam eder
- Prototip — üretim için uygun değil

---

## CLI Komutları (Normal Mod)

f/forward, b/backward, s/stop, pwm, kick on/off, ramp on/off, kickduty, kickms, ramprate, rampms, savecfg, loadcfg, defaults, saveall, hall, map, save, reload, mapreset, scan, test, identify, status, debug on/off, clrerr, mode, mode python/settings/normal

---

## Donanım Pinleri

Hall: PB6(H1), PB7(H2), PB8(H3)
Faz A: PA8(AH), PA7(AL)
Faz B: PA9(BH), PB0(BL)
Faz C: PA10(CH), PB1(CL)
LED: PC13
UART: PA2(TX), PA3(RX)

---

## Hızlı Referans

Tek dosya: src/main.cpp (2213 satır)
Modlar: Normal/Python/Settings
Protokol: WASD (geçici) → f/b/s (hedef)
Watchdog: 800ms, sadece Normal/Settings
EEPROM: Hall map(addr 0), config(addr 64), mod(addr 128)
Motor tick: 60µs
Dökümanlar: docs/ klasöründe
