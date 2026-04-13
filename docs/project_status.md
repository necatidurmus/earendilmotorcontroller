/*
 * project_status.md — Proje durum takip dosyası
 *
 * Bu dosya agent'ın kaldığı yerden devam etmesi için handoff dokümanıdır.
 * Her büyük milestone sonrası güncellenmelidir.
 * Son güncelleme: 2026-04-13
 */

# Earendil BLDC Motor Controller — Proje Durumu

## Sürüş Tipi
**Senkron komplementer PWM (6-step trapezoidal)**
- TIM1_CHx + TIM1_CHxN komplementer çiftler
- Donanım deadtime: BDTR.DTG=50 → ~521 ns MCU + ~300-400 ns L6388 = ~820-920 ns toplam
- Asenkron yapı tamamen kaldırıldı, bldc_commutation.c register seviyesinde çalışıyor

## Tamamlanan Modüller
- [x] `board_io.c` — SYSCLK 96 MHz, TIM1 komplementer PWM + deadtime, TIM3 kontrol, ADC1, USART2
- [x] `bldc_commutation.c` — 6-adım CCER lookup tablosu, register seviyesi komütasyon
- [x] `hall.c` — 7x majority vote, debounce, 4 profil, polarity mask, state offset, invalid hall hold
- [x] `protection.c` — EMA filtreli akım, soft/hard limit, fault latch, slew rate, offset kalibrasyonu
- [x] `cli.c` — Non-blocking UART2 CLI, 15+ komut, snapshot tabanlı raporlama
- [x] `stm32f4xx_it.c` — TIM3 IRQ → MotorControl_Tick(), HardFault/MemManage handler'ları
- [x] `motor_config.h` — Tüm pin/timer/ADC/koruma sabitleri, CLI_TRANSPORT seçimi
- [x] `usbd_conf.c/h` — USB CDC transport desteği
- [x] `stm32f4xx_hal_conf.h` — Sadece kullanılan HAL modülleri

## Eksik Modüller
- [ ] Throttle input (hardware mevcut ama firmware'de kullanılmıyor)
- [ ] Undervoltage koruması (VSENSE ölçeği doğrulanmadı)
- [ ] Termal koruma (NTC yok)
- [ ] RPM / hız geri bildirimi
- [ ] Closed-loop PI hız kontrolü
- [ ] DMA veya timer-triggered ADC (blocking ADC → ISR yükü azalt)
- [ ] TIM1 Break girişine OCP bağlantısı (donanım overcurrent trip)

## Teknik Riskler
1. **Clock doküman uyumsuzluğu**: architecture.md, config_reference.md, control_strategy.md hâlâ 100 MHz SYSCLK yazıyor. Kod 96 MHz.
2. **HardFault handler**: Motor sürücülü sistemde outputs kapatılmadan `while(1)` — MOSFET açık kalabilir.
3. **Blocking ADC**: ISR içinde blocking ADC okuma — jitter riski.
4. **INA181 gain biliniyor**: INA181A1 (gain=20 V/V). Akım tahmini güvenilir, koruma ADC delta üzerinde çalışıyor (doğru).

## Donanım Belirsizlikleri
| Konu | Durum |
|---|---|
| MOSFET | IRFB7730 — Vds(max)=75V, Rds(on)=3.1mΩ, Qg=137nC [DOĞRULANDI] |
| INA181 | A1 (INA181A1QDBVRQ1), gain=20 V/V [DOĞRULANDI] |
| VSENSE bölücü oranı | Teorik: R_top=47k, R_bot=2.2k → 0.04472; bench'te doğrulanmalı |
| Deadtime yeterliliği | ~820-920 ns teorik; osiloskopla doğrulanmalı |
| Hall profili | Profil 0 varsayılan; motora göre ayarlanmalı |

## Son Bırakılan Nokta
- Senkron komplementer PWM implementasyonu tamamlandı (feature branch)
- CLI ve hall modülleri güncellendi
- Dokümantasyon eklendi ama clock değerleri outdated
- Müdüne `cli.h`'de USART1/USART2 çelişkisi

## Bir Sonraki Yapılacaklar
1. Docs'taki clock değerlerini 96 MHz ile senkronize et
2. `cli.h` yorumundaki USART1 → USART2 düzeltmesi
3. HardFault handler'da BoardIO_AllOff() ekle
4. `project_status.md` ve `known_issues.md` oluştur
5. Build doğrulaması
