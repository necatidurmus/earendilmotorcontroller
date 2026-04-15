# Modül Referansı — Her Dosya Ne Yapar

## Header Dosyaları (`include/`)

---

### `motor_config.h`

**Proje genelinde tüm sabitler buradadır. Tek dokunma noktası.**

İçindekiler:
- Pin tanımları (hall, yüksek taraf, düşük taraf, ADC, LED)
- TIM1 PWM parametreleri (frekans, periyot, deadtime)
- TIM3 kontrol zamanlayıcısı parametreleri
- TIM4 hall sensor interface parametreleri
- ADC kanalları, örnekleme sabitleri
- Duty döngüsü limitleri ve rampa adımları
- Koruma eşikleri (soft/hard limit, strike sayısı)
- UART baud hızı, CLI tampon boyutu
- Hall profil sayısı ve extern bildirimi
- HAL handle extern bildirimleri (`htim1`, `htim3`, `htim4`, `hadc1`, `huart2`)

Donanım değişirse veya parametre ayarlanacaksa **yalnızca bu dosyaya bakılır**.

---

### `board_io.h`

**Donanım başlatma ve temel I/O API'si.**

Dışarıya açılan fonksiyonlar:

| Fonksiyon | Ne Yapar |
|---|---|
| `BoardIO_InitAll()` | Tüm alt sistemleri sırayla başlatır |
| `BoardIO_InitClock()` | 25 MHz HSE → 96 MHz SYSCLK PLL kurar |
| `BoardIO_InitGPIO()` | LED pin yapılandırması |
| `BoardIO_InitPWM()` | TIM1 komplementer PWM (6 kanal + deadtime) |
| `BoardIO_InitControlTimer()` | TIM3 ISR zamanlayıcısı |
| `BoardIO_StartControlTimer()` | TIM3'ü başlatır (motor kontrolü açılır) |
| `BoardIO_InitHallTimer()` | TIM4 Hall Sensor Interface (PB6/PB7/PB8 AF2) |
| `BoardIO_StartHallTimer()` | TIM4 capture interrupt'ı başlatır (Hall_Init sonrası) |
| `BoardIO_InitADC()` | PA0/PA4 analog giriş, ADC1 DMA |
| `BoardIO_InitUART()` | USART2 CLI (PA2/PA3) |
| `BoardIO_SetPWMx()` | TIM1 CCR doğrudan yaz |
| `BoardIO_AllOff()` | CCR=0, CCER=0 — güvenli durum |
| `BoardIO_ReadADC(ch)` | DMA tamponundan non-blocking ADC okuma |
| `BoardIO_LEDOn/Off/Toggle()` | LED kontrolü |
| `BoardIO_DelayUs(us)` | DWT tabanlı µs gecikme |

---

### `bldc_commutation.h`

**Komütasyon API'si ve `RunMode` enum tanımı.**

`RunMode` enum burada tanımlıdır — `main.c` ve `cli.c` bunu extern ile kullanır, kendi enum'larını tanımlamaz.

| Fonksiyon | Ne Yapar |
|---|---|
| `Comm_Init()` | Tüm çıkışları kapatır, durumu sıfırlar |
| `Comm_ApplyStep(state, duty, dir)` | ISR hot path — CCR + CCER yazar |
| `Comm_AllOff()` | CCR=0, CCER=0, activeState=0xFF |
| `Comm_GetActiveState()` | Son uygulanan durum (0..5 veya 0xFF) |
| `Comm_GetActiveDuty()` | Son uygulanan PWM duty (0..PWM_PERIOD_COUNTS) |

---

### `hall.h`

**Hall sensör okuma, debounce ve durum eşleştirme API'si.**

| Fonksiyon | Ne Yapar |
|---|---|
| `Hall_Init(cfg)` | Konfigürasyonla başlat |
| `Hall_SetProfile(p)` | Hall → durum eşleştirme profilini değiştir |
| `Hall_SetPolarityMask(m)` | Hall XOR maskesi (polarite düzeltme) |
| `Hall_SetStateOffset(o)` | Komütasyon durumu kaydırma (-5..+5) |
| `Hall_GetConfig(cfg)` | Mevcut konfigürasyonu oku |
| `Hall_ResolveState(nowUs)` | ISR çağrısı — son kabul edilen state + timeout kontrolü, 0..5 veya 255 döndür |
| `Hall_GetDriveState()` | Diagnostik sürüş durumu (yön uygulanmış) |
| `Hall_SetDirection(fwd)` | Yön ayarı (yalnızca diagnostik görüntüleme için) |
| `Hall_GetSnapshot(snap)` | CLI tanı anlık görüntüsü |
| `Hall_ReadRaw()` | Ham hall bit değeri |

`HallSnapshot` yapısı:
- `raw`: GPIO IDR'den okunan ham 3-bit değer
- `corrected`: polarityMask XOR sonrası
- `mapped`: profil tablosundan indekslenmiş durum (0..5 veya 255)
- `accepted`: debounce sonrası kabul edilen durum
- `driveState`: yön uygulaması sonrası diagnostik sürüş durumu
- `lastTransitionUs`: son hall geçiş zamanı (TIM4 zaman tabanı)
- `sectorPeriodUs`: son geçiş aralığı (µs)
- `stale`: timeout/stale durumu (1=geç veri)

---

### `protection.h`

**Akım ölçümü, koruma ve slew limiti API'si.**

| Fonksiyon | Ne Yapar |
|---|---|
| `Prot_Init(cfg)` | Konfigürasyonla başlat |
| `Prot_SampleTick()` | ISR çağrısı — ADC örnekle (decimated), EMA filtrele |
| `Prot_CheckHardLimit()` | Hard overcurrent → true ise fault latch'lendi |
| `Prot_ApplySoftLimit(duty)` | Soft limit aşıldıysa duty'yi azalt |
| `Prot_SlewDuty(current, target)` | Rampa limiti uygula |
| `Prot_CalibrateOffset()` | 128 örnek ortalamasıyla sıfır akım ofsetini öğren |
| `Prot_LatchFault(reason)` | Fault latch, AllOff() çağır |
| `Prot_ClearFault()` | Fault temizle (CLI 'clear' komutu) |
| `Prot_IsFaulted()` | Fault durumu sorgula |
| `Prot_GetFaultReason()` | Fault nedeni string'i |
| `Prot_SetLimits(soft, hard)` | Limitleri çalışma zamanında değiştir |
| `Prot_GetSnapshot(snap)` | CLI tanı anlık görüntüsü |
| `Prot_GetEstimatedAmps()` | Tahmini amper (SADECE görüntü için) |
| `Prot_SetInaGain(gain)` | INA181 kazanç düzeltmesi |

`ProtectionSnapshot` yapısı:
- `currentRaw`: son ham ADC değeri
- `currentFiltered`: EMA filtrelenmiş değer
- `currentOffset`: kalibrasyon ofset değeri
- `currentDelta`: filtered - offset (koruma kararları bu değere göre)
- `voltageRaw`: VSENSE ham ADC
- `estimatedAmps`: tahmini amper (INA181A1, gain=20 V/V — `gain` komutuyla ayarlanabilir)
- `softLimitActive`: soft limit aktif mi
- `hardStrikes`: ardışık hard limit aşım sayısı

---

### `cli.h`

**CLI başlatma ve servis API'si.**

| Fonksiyon | Ne Yapar |
|---|---|
| `CLI_Init()` | Satır tamponunu sıfırla, yardım menüsünü yazdır |
| `CLI_Service()` | Ana döngüden çağrılır — UART'tan byte oku, satır işle |

CLI, ISR'dan bağımsız çalışır. Motor kontrol state'ini `volatile` değişkenler üzerinden okur/yazar.

---

### `stm32f4xx_hal_conf.h`

**STM32Cube HAL modül seçimi.**

Hangi HAL modüllerinin derleneceğini belirler. İçeride yorum dışı bırakılan her `#define HAL_xxx_MODULE_ENABLED` satırı o modülün HAL kaynak dosyalarını derlemeye dahil eder.

Bu projede aktif modüller:
- `HAL_TIM_MODULE_ENABLED` — TIM1, TIM3 ve TIM4
- `HAL_ADC_MODULE_ENABLED` — ADC1
- `HAL_UART_MODULE_ENABLED` — USART2
- `HAL_IWDG_MODULE_ENABLED` — Watchdog
- `HAL_GPIO_MODULE_ENABLED`, `HAL_RCC_MODULE_ENABLED`, `HAL_CORTEX_MODULE_ENABLED`, `HAL_FLASH_MODULE_ENABLED`, `HAL_PWR_MODULE_ENABLED`, `HAL_DMA_MODULE_ENABLED`, `HAL_EXTI_MODULE_ENABLED`

Kullanılmayan modüller devre dışıdır → derleme boyutu küçüktür.

---

### `stm32f4xx_it.h`

**Kesme servis rutini (ISR) fonksiyon bildirimleri.**

`stm32f4xx_it.c`'de tanımlanan ISR'ların `extern` bildirimleri. CMSIS/HAL linker script'i bu isimleri vektör tablosunda bekler.

---

## Kaynak Dosyaları (`src/`)

---

### `main.c`

**Giriş noktası ve motor kontrol ISR çağrısı.**

- `main()`: tüm init çağrıları, kalibrasyon, CLI başlatma, ana döngü
- `MotorControl_Tick()`: TIM3 ISR tarafından 12.5 kHz'de çağrılır
  - ADC örnekle
  - Hard limit kontrol
  - Hall çöz
  - Soft limit uygula
  - Slew uygula
  - Comm_ApplyStep() çağır

Ana döngü: yalnızca `CLI_Service()` ve LED blink.

---

### `board_io.c`

**Tüm donanım başlatma implementasyonu.**

`BoardIO_InitPWM()` bu projedeki en kritik fonksiyon:
- PA7/PB0/PB1'i GPIO yerine TIM1_CH1N/CH2N/CH3N (AF1) olarak init eder
- PB6/PB7/PB8'i TIM4_CH1/CH2/CH3 (AF2) olarak init eder (hall sensor interface)
- 6 kanalı başlatır (3 yüksek taraf + 3 düşük taraf)
- BDTR deadtime ayarlar (DEADTIME_COUNTS = 20 → ~208 ns MCU tarafı)
- OSSR/OSSI = 1 (güvenli idle state)

---

### `bldc_commutation.c`

**6-adım senkron komplementer komütasyon implementasyonu.**

İki precomputed lookup tablosu:
- `CCER_FWD[6]` / `CCER_BWD[6]`: her adım için CCER değerleri
- `CCR_FWD_PTR[6]` / `CCR_BWD_PTR[6]`: aktif CCR pointer'ları

`Comm_ApplyStep()` her çağrıda 4 register yazar:
1. `TIM1->CCR1 = 0`
2. `TIM1->CCR2 = 0`
3. `TIM1->CCR3 = 0`
4. `*CCR_xxx_PTR[state] = duty`
5. `TIM1->CCER = CCER_xxx[state]`

HAL çağrısı yoktur. ISR hot path için optimize edilmiştir.

---

### `hall.c`

**TIM4 event-driven hall sensor capture, debounce ve profil eşleştirme.**

Mimari:
- TIM4 capture interrupt her hall geçişinde tetiklenir (BOTHEDGE)
- `HAL_TIM_IC_CaptureCallback()` çağrılır
- `readHallRawDirect()` GPIO IDR üzerinden hall bit'lerini okur (AF modunda da geçerli)
- Polarity mask, profil tablosu, debounce uygulanır
- Son kabul edilen state `lastAcceptedState`'te saklanır

TIM3 kontrol ISR'i `Hall_ResolveState()` ile yalnızca:
1. Son kabul edilen state'i döndürür
2. Timeout kontrolü yapar (`INVALID_HALL_HOLD_US`)
3. Polling veya majority vote YAPMAZ

Bu dosyada `HALL_TO_STATE_PROFILES[4][8]` tanımlıdır (motor_config.h'de extern).

---

### `protection.c`

**Akım koruma implementasyonu.**

ADC decimation ve EMA filtresi:
- Her 4. ISR tick'inde ADC örneklenir (`ADC_DECIMATION = 4`)
- `CURRENT_FILTER_ALPHA = 0.20` EMA katsayısı ile filtrelenir
- `currentDelta = filtered - offset`

Koruma kararları **ADC delta üzerinden** yapılır, asla `estimatedAmps` üzerinden değil.
INA181 kazancı bilinmediğinden `estimatedAmps` yalnızca CLI görüntüsü içindir.

---

### `cli.c`

**UART2 komut satırı arayüzü implementasyonu.**

Non-blocking okuma: `HAL_UART_Receive(..., timeout=0)` ile her `CLI_Service()` çağrısında mevcut byte'lar okunur, satır tamponuna eklenir.

Satır tamamlandığında `dispatch()` çağrılır → komut ayrıştırılır → ilgili handler.

CLI, ISR değişkenlerine yalnızca `volatile` pointer üzerinden erişir. Kritik bölge yoktur çünkü:
- 16/32-bit volatile yazımları Cortex-M4'te atomiktir
- CLI yalnızca `g_runMode` ve `g_commandDuty` yazar (ISR okur)
- ISR yalnızca `g_appliedDuty` ve `g_isrTickCount` yazar (CLI okur)

---

### `stm32f4xx_it.c`

**Kesme vektör implementasyonları.**

- `TIM3_IRQHandler`: TIM3 update flag'ini temizler, `MotorControl_Tick()` çağırır.
- `TIM4_IRQHandler`: TIM4 hall capture interrupt'ini HAL'a yönlendirir.

Diğer handler'lar (`HardFault_Handler`, `SysTick_Handler` vb.) varsayılan HAL implementasyonlarıdır.

---

## Yapılandırma Dosyaları

---

### `platformio.ini`

**PlatformIO derleme ortamı konfigürasyonu.**

| Alan | Değer | Açıklama |
|---|---|---|
| `platform` | `ststm32` | ST STM32 platform |
| `board` | `blackpill_f411ce` | WeAct Black Pill STM32F411CE |
| `framework` | `stm32cube` | STM32Cube HAL (Arduino yok) |
| `upload_protocol` | `stlink` | ST-Link programlayıcı |
| `debug_tool` | `stlink` | ST-Link debugger |
| `monitor_speed` | `115200` | CLI baud hızı |
| `USE_HAL_DRIVER` | define | HAL kütüphanesi aktif |
| `STM32F411xE` | define | Doğru MCU ailesi seçimi |
