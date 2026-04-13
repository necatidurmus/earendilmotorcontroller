# Sonraki Adımlar

## Hemen Yapılacak (Milestone: Clock Sync + Safety)

1. **`docs/architecture.md` clock düzeltmesi** — 100 MHz → 96 MHz
2. **`docs/config_reference.md` değer güncelleme** — PWM_PERIOD_COUNTS, prescaler
3. **`docs/control_strategy.md` deadtime düzeltmesi** — 500 ns → 521 ns
4. **`cli.h` USART1 → USART2 yorum düzeltmesi**
5. **HardFault handler'da output kapatma**
6. **`docs/modules.md` — usbd_conf.c/h ekleme**

## Kısa Vadede (Milestone: Protection Completion)

7. **VSENSE ölçeğini bench'te doğrula** → undervoltage koruması ekle
8. **DMA ADC'ye geçiş değerlendirmesi** — blocking ADC → ISR jitter azaltma
9. **TIM1 Break girişi** — donanım OCP sinyali gelirse BDTR.BreakState aktif et

## Orta Vadede (Milestone: Feature Complete)

10. **Throttle input** — hardware mevcut, ADC kanalını belirle, uygula
11. **RPM feedback** — hall geçiş süresi → hız tahmini
12. **Closed-loop PI** — hedef RPM veya duty için kapalı çevrim kontrol
13. **NTC termal koruma** — analog giriş ekle

## Uzun Vadede (Milestone: Production Ready)

14. **Watchdog timer** — IWDG ile sistem sağlığı kontrolü
15. **Configuration flash** — ayarları NVM'de sakla
16. **USB CDC full test** — UARTsız çalışma modu doğrulaması
17. **EMC / EMI test** — osiloskopla switching noise analizi

## Notlar
- Her milestone sonrası `project_status.md` güncelle
- Bench test sonuçlarını `docs/bringup.md` içine kaydet
- Build her değişiklikte doğrula: `pio run`
