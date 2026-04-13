# Bilinen Sorunlar ve Teknik Riskler

## Kod Sorunları

### 1. HardFault Handler Output Kapatmıyor
**Dosya:** `src/stm32f4xx_it.c:27-31`
**Sorun:** `HardFault_Handler` sadece `__disable_irq()` + `while(1)` yapıyor. Motor sürücülü sistemde bu, MOSFET'lerin son bilinen durumda kalmasına neden olabilir.
**Çözüm:** `BoardIO_AllOff()` veya doğrudan `TIM1->CCER = 0; TIM1->CCR1=0; TIM1->CCR2=0; TIM1->CCR3=0;` ekle.
**Risk:** Yüksek — güç aşamasında shoot-through riski.

### 2. `cli.h` Yorum Çelişkisi
**Dosya:** `include/cli.h:4` — "Uses UART (USART1)" diyor
**Gerçek:** `motor_config.h` ve `cli.c` USART2 (PA2/PA3) kullanıyor
**Risk:** Düşük — kod doğru çalışıyor, sadece yorum yanlış.

### 3. Blocking ADC ISR İçinde
**Dosya:** `src/protection.c:84-86` — `BoardIO_ReadADC()` blocking HAL_ADC_PollForConversion
**Sorun:** Her 4. ISR tick'te 2 kanal blocking ADC → ~7.7 µs jitter
**Risk:** Orta — 12.5 kHz ISR'da %9.6 CPU kullanımı. DMA veya timer-triggered ADC ile iyileştirilebilir.

## Dokümantasyon Sorunları

### 4. Clock Değerleri Outdated
**Etkilenen dosyalar:** `docs/architecture.md`, `docs/config_reference.md`, `docs/control_strategy.md`
**Sorun:** 100 MHz SYSCLK yazıyor. Kod 96 MHz (USB 48 MHz PLLQ gereksinimi).
**Etki:**
- `PWM_PERIOD_COUNTS`: 3332 → 3199
- `CTRL_TIMER_PRESCALER`: 99 → 95
- TIM1 clock: 200 MHz → 96 MHz
- Deadtime: 500 ns → 521 ns

### 5. USB CDC Dokümante Edilmemiş
**Sorun:** `CLI_TRANSPORT` mekanizması, `usbd_conf.c/h` mevcut ama hiçbir dokümanda bahsedilmemiş.

### 6. `modules.md` Eksik
**Sorun:** `usbd_conf.c` ve `usbd_conf.h` modülleri listelenmemiş.

## Donanım Belirsizlikleri

### 7. MOSFET Part Number Bilinmiyor
**Etki:** Rds(on), Qg, Vds(max) bilinmediği için termal ve SOA hesabı yapılamıyor.

### 8. INA181 Gain Suffix Bilinmiyor
**Etki:** `estimatedAmps` gösterimi yaklaşık. Koruma ADC delta üzerinde çalışıyor (doğru — suffix bağımsız).
**Çözüm:** PCB üzerindeki markaj okunmalı.

### 9. VSENSE Bölücü Oranı Teorik
**Etki:** `VSENSE_DIVIDER_RATIO = 0.04472f` şematik R değerlerinden hesaplanmış. Gerçek R değerleri ölçülmeli.
**TODO:** `motor_config.h:242` — undervoltage eşiği bu doğrulandıktan sonra eklenecek.

### 10. Deadtime Yeterliliği Doğrulanmadı
**Hesap:** ~521 ns MCU + ~300-400 ns L6388 dahili = ~820-920 ns
**Çözüm:** Osiloskopla PA8/PA7 çiftinde cross-conduction kontrolü.
