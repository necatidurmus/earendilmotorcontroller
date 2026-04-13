# Bilinen Sorunlar ve Teknik Riskler

## Kod Sorunları

### 1. ~~HardFault Handler Output Kapatmıyor~~ ✅ Düzeltildi
**Dosya:** `src/stm32f4xx_it.c:27-36`
**Durum:** `HardFault_Handler` artık `TIM1->CCER=0; CCR1/2/3=0` ile tüm çıkışları kapatıyor.

### 2. ~~`cli.h` Yorum Çelişkisi~~ ✅ Düzeltildi
**Dosya:** `include/cli.h:4`
**Durum:** Yorum artık "Uses USART2 (PA2=TX, PA3=RX)" diyor.

### 3. ~~Blocking ADC ISR İçinde~~ ✅ İyileştirildi
**Dosya:** `src/board_io.c` — `BoardIO_ReadADC()`
**Durum:** `HAL_ADC_PollForConversion` yerine register-level EOC polling kullanılıyor. HAL timeout/mutex overhead kaldırıldı. Kanal değişmeyince yeniden yapılandırma atlanıyor.

### 4. [CRITICAL] SysTick Öncelik Motor Kontrol ISR ile Aynı Seviyede
**Dosya:** `include/stm32f4xx_hal_conf.h:55`, `src/board_io.c:288`
**Sorun:** `TICK_INT_PRIORITY = 0` (en yüksek) ve `TIM3_IRQn` önceliği de `0`. Cortex-M4'te aynı seviyedeki kesmeler birbirini preempt edemez. Motor kontrol ISR çalışırken SysTick gecikir; SysTick çalışırken motor kontrol ISR gecikir. `HAL_GetTick()` kayması, motor timing jitter, UART timeout hataları oluşur. `docs/architecture.md` satır 88 "SysTick = 15 (en düşük)" diyor ama kodda `0`.

### 5. [CRITICAL] Volatile Olmayan ISR Durum Değişkenleri
**Dosya:** `src/protection.c:41-44`
**Sorun:** `faultLatched`, `softLimitActive`, `hardStrikes`, `faultReason` değişkenleri ISR'dan yazılıp CLI'dan okunuyor ama `volatile` bildirilmemiş. Derleyici register'da cache'leyebilir, CLI tarafında güncel değeri okuyamayabilir. `Prot_IsFaulted()` çağrısında stale değer dönme riski.

### 6. [CRITICAL] cliPrintInt INT32_MIN'de Tanımsız Davranış
**Dosya:** `src/cli.c:79`
**Sorun:** `val = -val` ifadesi `val == INT32_MIN` (-2147483648) olduğunda signed integer overflow oluşturur. C standardında tanımsız davranış. Şu an sadece `-5..+5` aralığında tetiklenmez ama fonksiyon genel amaçlı.

### 7. [HIGH] FaultState Struct Tanımlanmış Ama Kullanılmıyor
**Dosya:** `include/protection.h:45-48`
**Sorun:** `FaultState` typedef'i (latched + reason[48]) tanımlı, hiçbir .c dosyasında kullanılmıyor. Half-bırakılmış refactor artığı. Dead code.

### 8. [MEDIUM] cliPrintFloat Buffer Overflow Riski
**Dosya:** `src/cli.c:93-106`
**Sorun:** `buf[8]` tanımlı. `decimals > 7` olursa buffer taşar. Şu an `decimals=2` ile çağrılıyor, aktif değil ama fonksiyon genel amaçlı ve gelecekteki kullanımlarda tehlikeli.

### 9. [MEDIUM] ADC EOC Polling Timeout Yok — ISR Lockup Riski
**Dosya:** `src/board_io.c:352`
**Sorun:** `while (!(hadc1.Instance->SR & ADC_SR_EOC)) { }` döngüsünde timeout yok. ADC donanım arızasında EOC asla set edilmez, motor kontrol ISR sonsuz döngüde kalır, sistem kilitlenir. TIM3 en yüksek öncelikte, watchdog da resetleyemez.

### 10. [MEDIUM] EMA Filtre Init Koşulu Sorunlu
**Dosya:** `src/protection.c:88-91`
**Sorun:** `if (currentFiltered < 1.0f)` filtre init belirleme olarak kullanılıyor. Offset kalibrasyonu 0 olursa ve ilk ADC okuma 0'sa, filtre her seferinde sıfırlanır. EMA filtresi düzgün çalışmaz. Ayrı bir `bool filterInitialized` flag'i gerekli.

### 11. [MEDIUM] nowUs Overflow — ~72 Dakika Sonra
**Dosya:** `src/main.c:78`
**Sorun:** `uint32_t nowUs = g_isrTickCount * 80U;` 12.5 kHz ISR'da ~72 dakika sonra overflow yapar. Hall debounce/timeout hesapları bozulur. 64-bit veya wrap-around handling gerekli.

### 12. [MEDIUM] gain Help Mesajı ile Kod Uyumsuz
**Dosya:** `src/cli.c:336, 376`
**Sorun:** Help mesajı "Usage: gain <20|50|100|200>" diyor. Kod `atol(arg)` ile 1-1000 aralığını kabul ediyor. Kullanıcı kafa karıştırır.

### 13. [MEDIUM] CCER Register'a Direkt Atama — Reserved Bit Riski
**Dosya:** `src/bldc_commutation.c:132, 135`
**Sorun:** `TIM1->CCER = CCER_FWD[state];` direkt atama. HAL bu bitleri başka amaçla kullanıyorsa bozulabilir. Daha güvenli: `TIM1->CCER = (TIM1->CCER & ~CCER_ALL_MASK) | CCER_FWD[state];`

### 14. [MEDIUM] platformio.ini build_unflags Anlamsız
**Dosya:** `platformio.ini:26`
**Sorun:** `build_unflags = -fno-rtti` RTTI etkinleştirmeye çalışır ama proje saf C. İşlevsiz. "Use newlib-nano" yorumu var ama `--specs=nano.specs` eklenmemiş.

## Dokümantasyon Sorunları

### 15. ~~[HIGH] motor_config.h Clock Yorumları 100 MHz — Gerçek 96 MHz~~ ✅ Düzeltildi
**Dosya:** `include/motor_config.h:100-121`
**Durum:** Clock yorumları 96 MHz'e güncellendi (SYSCLK=96, APB1=48, APB2=96, TIM1=96 MHz, TIM3=96 MHz, PSC=0, ARR=3199, deadtime=10.4 ns).

### 16. ~~[HIGH] motor_config.h Yorumlarında Arduino Framework Değerleri~~ ✅ Düzeltildi
**Dosya:** `include/motor_config.h:103, 107-114`
**Durum:** Arduino framework referansları kaldırıldı. Tüm timer yorumları STM32Cube (96 MHz) değerlerini yansıtıyor.

### 17. ~~[HIGH] board_io.c ADC Clock Yorum Hataları~~ ✅ Düzeltildi
**Dosya:** `src/board_io.c:302-309`
**Durum:** ADC clock yorumları düzeltildi: PCLK2=96 MHz, ADC=24 MHz, ~4 us/okuma, ~0.5-1 us/tick.

### 18. ~~[LOW] board_io.h Yorumunda 100 MHz SYSCLK~~ ✅ Düzeltildi
**Dosya:** `include/board_io.h:5`
**Durum:** "96 MHz" olarak düzeltildi.

### 19. ~~[LOW] known_issues.md Clock Değerleri Outdated~~ ✅ Düzeltildi
**Dosya:** `docs/known_issues.md`
**Durum:** Bu madde motor_config.h düzeltmeleriyle otomatik olarak geçersizleşti. Clock yorumları artık 96 MHz doğru değerleri yansıtıyor.

### 20. [LOW] Hall Pinleri GPIO_SPEED_FREQ_HIGH — Gereksiz EMI
**Dosya:** `src/board_io.c:134`
**Sorun:** Hall sensör pinleri (PB6/PB7/PB8) HIGH_SPEED'de. Hall sinyali tipik <1 kHz. LOW yeterli, HIGH gereksiz EMI ve güç tüketimi.

### 21. [LOW] Prot_ClearFault strncpy Null-termination Eksik
**Dosya:** `src/protection.c:181`
**Sorun:** `strncpy(faultReason, "none", sizeof(faultReason) - 1)` null-termination satırı yok. strncpy bu durumda 0 doldurduğu için teknik olarak güvenli ama explicit null-termination daha güvenli pattern.

### 22. [LOW] VSENSE Bölücü Oranı R12/R13 Belirsiz
**Dosya:** `include/motor_config.h:198`
**Sorun:** `VSENSE_DIVIDER_RATIO` yorumunda "R12=47k, R13=2.2k" PCB şematik numaraları, kod açısından belirsiz. Oran teorik, bench'te doğrulanmamış.

### 23. [LOW] RCC GPIOA Clock Enable Tekrarlı Çağrı
**Dosya:** `src/board_io.c:116, 366`
**Sorun:** `BoardIO_InitGPIO()` ve `BoardIO_InitUART()` ikisi de `__HAL_RCC_GPIOA_CLK_ENABLE()` çağırıyor. HAL idare eder ama gereksiz.

## Donanım/Güvenlik Eksiklikleri

### 24. [LOW] IWDG Watchdog Timer Eksik
**Sorun:** `docs/next_steps.md`'de planlanıyor ama henüz uygulanmadı. ISR veya main loop takılırsa motor açık kalabilir. HardFault handler var ama hangup durumunda koruma yok.

### 25. [LOW] Undervoltage ve Termal Koruma Eksik
**Dosya:** `include/motor_config.h:237-238`
**Sorun:** VSENSE ADC'den okunuyor ama koruma yapılmıyor. Düşük voltajda L6388 gate drive yetersiz olabilir. NTC termal koruma henüz eklenmemiş.

### 26. [LOW] MOE Biti Runtime Check Eksik
**Dosya:** `src/board_io.c:250-262`
**Sorun:** `BoardIO_InitPWM()` HAL çağrıları ile MOE set edilir, sonra CCR/CCER temizlenir. Sıralama doğru ama MOE'nin açık olduğu runtime'da garanti edilmemiş. `assert` veya check eklenmeli.

### 27. [MEDIUM] Hall_SetDirection Yorum-Kod Çelişkisi
**Dosya:** `include/hall.h:62`
**Sorun:** Yorum "0=forward (state as-is), 1=backward (state+3)" diyor ama kod `forward ? 0 : 1` ile tersini yapıyor. `Hall_SetDirection(1)` çağrıldığında `driveDirection=0` (ileri) oluyor, yorum "1=backward" diyor. Kod doğru, yorum yanlış.

### 28. [LOW] Hall_GetDriveState() Ölü Kod
**Dosya:** `src/hall.c:164-173`
**Sorun:** `Hall_GetDriveState()` fonksiyonu tanımlı ama ISR tarafından hiç çağrılmıyor. Yön mantığı `Comm_ApplyStep()` üzerinden `bldc_commutation.c`'de yapılıyor. Sadece CLI tanılama snapshot'ında kullanılıyor.

### 29. [LOW] cli.c Gereksiz extern huart2 Bildirimi
**Dosya:** `src/cli.c:48`
**Sorun:** `extern UART_HandleTypeDef huart2;` bildirimi gereksiz. `motor_config.h` (cli.c tarafından include ediliyor) zaten aynı extern bildirimini veriyor. Duplicate bildirim.

### 30. [LOW] Dead-Time State Geçişi Kaba (Slew All-Off)
**Dosya:** `src/main.c` — `MotorControl_Tick()`
**Sorun:** Hall değişimi algılandığında bir tam ISR periyodu boyunca tüm çıkışlar kapatılıyor (all-off), sonra yeni step uygulanıyor. Güvenli ama kaba — bir ISR periyodu (80 us) boyunca motor enerjisiz kalıyor. Sürüş kalitesi ve verim açısından rafine edilebilir.

## Donanım Bilgileri

### 31. MOSFET: IRFB7730 — Doğrulandı
**Part:** IRFB7730 (Infineon IRF7730 equivalent)
**Parametreler:** Vds(max)=75V, Rds(on)=3.1mΩ @Vgs=10V, Qg=137nC, Id=120A
**Durum:** Part number biliniyor. Termal ve SOA hesabı yapılabilir.

### 32. INA181A1 — Gain Doğrulandı
**Part:** INA181A1QDBVRQ1 (TI)
**Gain:** 20 V/V (A1 varyantı)
**Durum:** `INA_GAIN_DEFAULT = 20.0f` olarak güncellendi. Shunt 0.5 mΩ ile tam akım aralığı hesaplanabilir.

### 33. VSENSE Bölücü Oranı Doğrulandı
**Değer:** `VSENSE_DIVIDER_RATIO = 0.04472f` — R_top=47k, R_bot=2.2k → 2.2/(47+2.2)
**Durum:** Doğru, teorik değer korundu. Bench'te multimetre ile doğrulanmalı.

### 34. Deadtime Yeterliliği Doğrulanmadı
**Hesap:** ~521 ns MCU + ~300-400 ns L6388 dahili = ~820-920 ns
**IRFB7730 Qg=137nC:** Gate şarj süresi deadtime bütçesine dahil edilmeli.
**Çözüm:** Osiloskopla PA8/PA7 çiftinde cross-conduction kontrolü.
