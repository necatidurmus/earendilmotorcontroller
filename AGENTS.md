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
src/main.cpp              — tüm firmware (2324 satır, tek dosya)
tools/wasd_controller.py  — Python host (f/b/s UI)
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
- 3 çalışma modu: Normal (CLI), Python (f/b/s), Settings (temiz monitör)
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
7. checkCommandWatchdog()        — failsafe (tüm modlarda)
```

---

## UART Protokolü

### f/b/s Protokolü

f=ileri varsayılan PWM, f<duty>=ileri duty(0-255), b=geri varsayılan PWM, b<duty>=geri duty, s=dur (coast stop)

Lease: Her hareket komutu zaman damgasını yeniler. 800ms komut gelmezse motor durur.

### Telemetri Formatı

```
RPM:0,D:0,DIR:F,PH:2,PWM_SET:150,PWM_ACT:0,PDIR:1,H:3
```

RPM=devir, D=mevcut duty, DIR=F/R, PH=faz(0-4), PWM_SET=hedef duty, PWM_ACT=firmware duty, PDIR=yön(1/-1/0), H=ham Hall

---

## Kritik Bilinen Sorunlar

| Sorun | Öncelik | Durum |
|-------|---------|-------|
| (yok) | — | Tüm kritik sorunlar çözüldü |

Detay: docs/ISSUES.md

---

## Geliştirme Workflow'u

Git tag'leri her faz bitiminde:
- v0.1.0 — Dokümantasyon baseline
- v0.4.0 — Phase 1+3 (stabilize, watchdog/failsafe)
- v0.5.0 — Phase 2 (f/b/s protokolü)

Faz sırası: Stabilize → f/b/s → Lease → Brake → Cleanup → Multi-Motor → Tank Steering

---

## Güvenlik Uyarıları

- Motor çalışıyor! Test sırasında fiziksel acil durdurma hazır olmalı
- L6388 351ns dead-time sağlıyor — software dead-time gerekli değil
- Donanımsal watchdog (IWDG) 500ms — firmware takılırsa MCU reset
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

Tek dosya: src/main.cpp (2324 satır)
Modlar: Normal/Python/Settings
Protokol: f/b/s
Watchdog: 800ms, tüm modlarda aktif
EEPROM: Hall map(addr 0), config(addr 64), mod(addr 128)
Motor tick: 60µs
Dökümanlar: docs/ klasöründe
