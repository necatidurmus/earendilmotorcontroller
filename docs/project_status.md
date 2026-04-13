/*
 * project_status.md — Proje durum takip dosyasi
 * Son guncelleme: 2026-04-13
 */

# Earendil BLDC Motor Controller — Proje Durumu

## Surus Tipi
**Senkron komplementer PWM (6-step trapezoidal)**
- TIM1_CHx + TIM1_CHxN komplementer ciftler
- Donanim deadtime: BDTR.DTG=50
- Komutasyon: CCER lookup + register-level hot path

## Tamamlanan Moduller
- [x] `board_io.c` — SYSCLK 96 MHz, TIM1 komplementer PWM, TIM3 kontrol, ADC1, UART2
- [x] `bldc_commutation.c` — 6-adim CCER lookup tablosu
- [x] `hall.c` — majority vote, debounce, 4 profil, mask/offset
- [x] `protection.c` — EMA, soft/hard limit, fault latch, slew
- [x] `cli.c` — UART CLI
- [x] `stm32f4xx_it.c` — TIM3 ISR route
- [x] `IWDG watchdog` — init + main loop refresh
- [x] `Undervoltage fault` — strike + histerezis + latch + CLI runtime config

## Acik Isler
- [ ] RPM / hiz geri bildirimi
- [ ] Closed-loop PI hiz kontrolu
- [ ] TIM1 Break girisine OCP baglantisi

## Ertelenen Isler
- [ ] ADC DMA veya timer-triggered ADC (erken)

## Teknik Riskler
1. ADC okuma ISR icinde polling (timeout var, yine de DMA daha iyi)
2. Deadtime yeterliligi osiloskopla dogrulanmadi
3. VSENSE bolucu orani sahada dogrulanmadi (undervoltage esigi buna bagli)
4. State gecisinde bir ISR periyodu all-off (guvenli ama kaba)

## Not
- UART default transport aktif: `CLI_TRANSPORT_UART`
- Son derleme: `pio run` basarili
