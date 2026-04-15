/*
 * project_status.md — Proje durum takip dosyasi
 * Son guncelleme: 2026-04-13
 */

# Earendil BLDC Motor Controller — Proje Durumu

## Surus Tipi
**Senkron komplementer PWM (6-step trapezoidal)**
- TIM1_CHx + TIM1_CHxN komplementer ciftler
- Donanim deadtime: BDTR.DTG=20 (~208 ns)
- Komutasyon: CCER lookup + register-level hot path

## Tamamlanan Moduller
- [x] `board_io.c` — SYSCLK 96 MHz, TIM1 komplementer PWM, TIM3 kontrol, TIM4 hall, ADC1 DMA, UART2
- [x] `bldc_commutation.c` — 6-adim CCER lookup tablosu
- [x] `hall.c` — TIM4 event-driven hall capture, debounce, 4 profil, mask/offset
- [x] `protection.c` — EMA, soft/hard limit, fault latch, slew
- [x] `cli.c` — UART CLI
- [x] `stm32f4xx_it.c` — TIM3 ISR route + TIM4 hall capture ISR
- [x] `IWDG watchdog` — init + main loop refresh
- [x] `Undervoltage fault` — strike + histerezis + latch + CLI runtime config

## Acik Isler
- [ ] RPM / hiz geri bildirimi
- [ ] Closed-loop PI hiz kontrolu
- [ ] TIM1 Break girisine OCP baglantisi

## Ertelenen Isler
- [ ] ADC DMA veya timer-triggered ADC (erken)

## Teknik Riskler
1. Deadtime yeterliligi osiloskopla dogrulanmadi
2. VSENSE bolucu orani sahada dogrulanmadi (undervoltage esigi buna bagli)
3. State gecisinde CCER/CCR shadow register sync (deadtime korur)

## Not
- UART default transport aktif: `CLI_TRANSPORT_UART`
- Son derleme: `pio run` basarili
