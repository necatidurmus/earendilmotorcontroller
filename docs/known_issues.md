# Bilinen Sorunlar ve Teknik Riskler

## Kod Sorunları

### 1. ~~HardFault Handler Output Kapatmıyor~~ ✅ Düzeltildi
**Durum:** `HardFault_Handler` `TIM1->CCER=0; CCR1/2/3=0` ile tüm çıkışları kapatıyor.

### 2. ~~`cli.h` Yorum Çelişkisi~~ ✅ Düzeltildi
**Durum:** Yorum "Uses USART2 (PA2=TX, PA3=RX)" diyor.

### 3. ~~Blocking ADC ISR İçinde~~ ✅ İyileştirildi
**Durum:** Register-level EOC polling + timeout. HAL overhead kaldırıldı.

### 4. ~~[CRITICAL] SysTick Öncelik Çakışması~~ ✅ Düzeltildi
**Durum:** `TICK_INT_PRIORITY = 15` (en düşük). TIM3 priority 0. Artık preempt edebilir.

### 5. ~~[CRITICAL] Volatile Olmayan ISR Değişkenleri~~ ✅ Düzeltildi
**Durum:** `faultLatched`, `softLimitActive`, `hardStrikes` artık `volatile`.

### 6. ~~[CRITICAL] cliPrintInt INT32_MIN UB~~ ✅ Düzeltildi
**Durum:** Overflow-safe negation guard eklendi.

### 7. ~~[HIGH] FaultState Dead Code~~ ✅ Düzeltildi
**Durum:** `FaultState` struct silindi.

### 8. ~~[MEDIUM] cliPrintFloat Buffer Overflow~~ ✅ Düzeltildi
**Durum:** `buf[8]` → `buf[16]`.

### 9. ~~[MEDIUM→HIGH] ADC EOC Timeout Yok~~ ✅ Düzeltildi
**Durum:** `~10 us` timeout ile ISR lockup önlendi.

### 10. ~~[MEDIUM] EMA Filtre Init Koşulu~~ ✅ Düzeltildi
**Durum:** `bool filterInitialized` flag eklendi.

### 11. ~~[MEDIUM] nowUs Overflow~~ ✅ Düzeltildi
**Durum:** 64-bit multiply ile overflow önlendi.

### 12. ~~[MEDIUM] gain Help Uyumsuz~~ ✅ Düzeltildi
**Durum:** Help "1..1000", kodla tutarlı.

### 13. ~~[MEDIUM] CCER Direkt Atama~~ ✅ Düzeltildi
**Durum:** `CCER_COMM_MASK` ile read-modify-write.

### 14. ~~[MEDIUM] platformio.ini build_unflags~~ ✅ Düzeltildi
**Durum:** Anlamsız `build_unflags` satırı kaldırıldı.

### 15. ~~[HIGH] motor_config.h Clock Yorumları~~ ✅ Düzeltildi
**Durum:** 96 MHz doğru değerler.

### 16. ~~[HIGH] Arduino Framework Referansları~~ ✅ Düzeltildi
**Durum:** Tüm yorumlar STM32Cube 96 MHz.

### 17. ~~[HIGH] board_io.c ADC Clock Yorumları~~ ✅ Düzeltildi
**Durum:** PCLK2=96, ADC=24 MHz.

### 18. ~~[LOW] board_io.h SYSCLK Yorumu~~ ✅ Düzeltildi
**Durum:** "96 MHz".

### 19. ~~[LOW] known_issues.md Outdated~~ ✅ Düzeltildi
**Durum:** Clock değerleri tutarlı.

### 20. ~~[LOW] Hall Pin Speed~~ ✅ Düzeltildi
**Durum:** `GPIO_SPEED_FREQ_LOW`.

### 21. ~~[LOW] Prot_ClearFault Null-termination~~ ✅ Düzeltildi
**Durum:** Explicit `'\0'` eklendi.

### 22. ~~[LOW] RCC GPIOA Tekrarlı~~ ✅ Düzeltildi
**Durum:** `InitUART`'daki duplicate `__HAL_RCC_GPIOA_CLK_ENABLE()` kaldırıldı.

### 23. ~~[LOW] MOE Runtime Check~~ ✅ Düzeltildi
**Durum:** PWM init sonrası MOE assert eklendi.

## Açık Sorunlar

### 24. [LOW] IWDG Watchdog Timer Eksik
**Sorun:** ISR veya main loop takılırsa motor açık kalabilir. HardFault handler var ama hangup durumunda koruma yok.

### 25. [LOW] Undervoltage ve Termal Koruma Eksik
**Dosya:** `include/motor_config.h:237-238`
**Sorun:** VSENSE ADC'den okunuyor ama koruma yapılmıyor. NTC termal koruma henüz eklenmemiş.

### 26. [LOW] VSENSE Bölücü Oranı Teorik
**Dosya:** `include/motor_config.h:198`
**Sorun:** `VSENSE_DIVIDER_RATIO = 0.04472f` teorik, bench'te doğrulanmamış.

### 27. [LOW] Dead-Time State Geçişi Kaba (Slew All-Off)
**Dosya:** `src/main.c` — `MotorControl_Tick()`
**Sorun:** Hall değişimi algılandığında bir tam ISR periyodu (~80 us) all-off. Güvenli ama kaba. `docs/control_strategy.md`'de belgelendi.

### 28. [LOW] Deadtime Yeterliliği Doğrulanmadı
**Hesap:** ~521 ns MCU + ~300-400 ns L6388 dahili = ~820-920 ns
**Çözüm:** Osiloskopla PA8/PA7 çiftinde cross-conduction kontrolü.

## Donanım Bilgileri

### 29. MOSFET: IRFB7730 — Doğrulandı
**Part:** IRFB7730, Vds(max)=75V, Rds(on)=3.1mΩ, Qg=137nC, Id=120A

### 30. INA181A1 — Gain Doğrulandı
**Part:** INA181A1QDBVRQ1, Gain=20 V/V

### 31. VSENSE Bölücü Oranı Doğrulandı
**Değer:** `VSENSE_DIVIDER_RATIO = 0.04472f` — R_top=47k, R_bot=2.2k
