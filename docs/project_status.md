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
- CCER lookup tablosu, register seviyesi komütasyon (HAL yok ISR'da)

## Tamamlanan Modüller
- [x] `board_io.c` — SYSCLK 96 MHz, TIM1 komplementer PWM + deadtime, TIM3 kontrol, ADC1 (EOC timeout), USART2
- [x] `bldc_commutation.c` — 6-adım CCER lookup tablosu, CCER_COMM_MASK read-modify-write
- [x] `hall.c` — 7x majority vote, debounce, 4 profil, polarity mask, state offset, invalid hall hold
- [x] `protection.c` — EMA filtre (filterInitialized), soft/hard limit, fault latch (volatile state), slew rate, offset kalibrasyonu
- [x] `cli.c` — Non-blocking UART2 CLI, 15+ komut, INT32_MIN safe, buf[16] float print
- [x] `stm32f4xx_it.c` — TIM3 IRQ → MotorControl_Tick(), HardFault/MemManage handler'ları
- [x] `motor_config.h` — Tüm pin/timer/ADC/koruma sabitleri, clock yorumları 96 MHz doğru
- [x] `usbd_conf.c/h` — USB CDC transport desteği
- [x] `stm32f4xx_hal_conf.h` — TICK_INT_PRIORITY=15, sadece kullanılan HAL modülleri

## Eksik Modüller
- [ ] Throttle input (hardware mevcut ama firmware'de kullanılmıyor)
- [ ] Undervoltage koruması (VSENSE ölçeği doğrulanmadı)
- [ ] Termal koruma (NTC yok)
- [ ] RPM / hız geri bildirimi
- [ ] Closed-loop PI hız kontrolü
- [ ] DMA veya timer-triggered ADC (blocking ADC → ISR yükü azalt)
- [ ] TIM1 Break girişine OCP bağlantısı (donanım overcurrent trip)
- [ ] IWDG watchdog timer

## Teknik Riskler
1. **ADC blocking EOC**: Timeout eklendi (~10 us) ama hâlâ ISR içinde. DMA'ye geçiş değerlendirilmeli.
2. **Deadtime doğrulanmadı**: ~820-920 ns teorik, osiloskop gerekli.
3. **VSENSE oranı teorik**: Bench'te multimetre ile doğrulanmalı.
4. **State geçiş slew**: Bir tam ISR periyodu all-off (güvenli ama kaba). `control_strategy.md`'de belgelendi.

## Donanım Belirsizlikleri
| Konu | Durum |
|---|---|
| MOSFET | IRFB7730 — Vds(max)=75V, Rds(on)=3.1mΩ, Qg=137nC [DOĞRULANDI] |
| INA181 | A1 (INA181A1QDBVRQ1), gain=20 V/V [DOĞRULANDI] |
| VSENSE bölücü oranı | Teorik: R_top=47k, R_bot=2.2k → 0.04472; bench'te doğrulanmalı |
| Deadtime yeterliliği | ~820-920 ns teorik; osiloskopla doğrulanmalı |
| Hall profili | Profil 0 varsayılan; motora göre ayarlanmalı |

## Phase 0 Tamamlama Durumu
Tüm 23 madde tamamlandı (2026-04-13). Ayrı commit'ler ile kayıtlı:
- CRITICAL: SysTick priority, volatile ISR vars, INT32_MIN guard
- MEDIUM: nowUs overflow, ADC EOC timeout, cliPrintFloat, EMA filter, gain help, CCER RMW, FaultState, build_unflags
- LOW: Hall_SetDirection yorum, Hall_GetDriveState diagnostic, extern huart2, MOE assert, clock doc, null-termination, hall pin speed, RCC duplicate, slew doc
