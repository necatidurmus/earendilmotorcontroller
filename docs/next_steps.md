# Sonraki Adımlar

## Phase 1: Ölçüm Gerektiren — Konfigürasyon Kalibrasyonu

Ölçüm yapıldıktan sonra `motor_config.h` sabitleri güncellenecek.

1. **Hall ve komütasyon doğrulama** — düşük duty'de motor döndür, hall raw/corrected/mapped izle, doğru profili bul
2. **Akım kalibrasyonu** — `zeroi`, no-load dlt, stall dlt, soft/hard limit belirle
3. **Duty ve ramp kalibrasyonu** — DUTY_MIN_ACTIVE, max safe duty, ramp step limitleri
4. **Analog ölçekleme** — INA181 gain doğrula, VSENSE multimetre ile karşılaştır
5. **Deadtime doğrulama** — osiloskopla PA8/PA7 cross-conduction kontrolü

## Phase 2: Ölçüm Sonrası Mimari Kararlar

6. **ADC DMA'ye geçiş değerlendirmesi** — ISR blocking ADC jitter riski
7. **USB CDC'ye geçiş** — UART bring-up tamamlanınca
8. **TIM1 Break girişi** — donanım OCP hattı netleşince

## Phase 3: Güvenlik ve Feature Eksiklikleri

9. **IWDG watchdog timer** — ISR/main loop takılmasına karşı koruma
10. **Undervoltage koruması** — VSENSE doğrulanınca eklenecek
11. **Termal koruma (NTC)** — NTC donanımı eklenince
12. **Throttle input** — hardware mevcut, ADC kanalını belirle
13. **RPM feedback** — hall geçiş süresi → hız tahmini
14. **Closed-loop PI** — hedef RPM için kapalı çevrim kontrol

## Notlar
- Her milestone sonrası `project_status.md` güncelle
- Bench test sonuçlarını `docs/bringup.md` içine kaydet
- Build her değişiklikte doğrula: `pio run`
