

# Konfigürasyon Referansı — `motor_config.h`

Donanım değişikliği veya parametre ayarı için tek dokunma noktası. Her sabitin ne yaptığı, hangi hesaplamadan çıktığı ve nasıl ayarlanacağı aşağıda açıklanmıştır.

---

## Pin Tanımları

### Yüksek Taraf PWM (TIM1 komplementer çıkış, AF1)

| Sabit | Pin | TIM1 Kanal | L6388 |
|---|---|---|---|
| `AH_PIN = GPIO_PIN_8` | PA8 | CH1 | INH_A |
| `BH_PIN = GPIO_PIN_9` | PA9 | CH2 | INH_B |
| `CH_PIN = GPIO_PIN_10` | PA10 | CH3 | INH_C |

### Düşük Taraf PWM (TIM1 komplementer N çıkış, AF1)

| Sabit | Pin | TIM1 Kanal | L6388 |
|---|---|---|---|
| `AL_PIN = GPIO_PIN_7` | PA7 | CH1N | INL_A |
| `BL_PIN = GPIO_PIN_0` | PB0 | CH2N | INL_B |
| `CL_PIN = GPIO_PIN_1` | PB1 | CH3N | INL_C |

> Bu pinler GPIO DEĞİL. `board_io.c`'de AF1 modunda başlatılır.

### Hall Sensörleri

| Sabit | Pin | Açıklama |
|---|---|---|
| `HALL_A_PIN = GPIO_PIN_6` | PB6 | Faz A hall |
| `HALL_B_PIN = GPIO_PIN_7` | PB7 | Faz B hall |
| `HALL_C_PIN = GPIO_PIN_8` | PB8 | Faz C hall |

### Analog Girişler

| Sabit | Pin | ADC Kanal | Bağlantı |
|---|---|---|---|
| `ISENSE_ADC_PIN = GPIO_PIN_0` | PA0 | `ISENSE_ADC_CHANNEL = ADC_CHANNEL_0` | INA181 çıkışı |
| `VSENSE_ADC_PIN = GPIO_PIN_4` | PA4 | `VSENSE_ADC_CHANNEL = ADC_CHANNEL_4` | Voltaj bölücü |

### LED

| Sabit | Pin | Davranış |
|---|---|---|
| `LED_PIN = GPIO_PIN_13` | PC13 | Aktif düşük (SET=kapalı, RESET=açık) |

---

## Timer ve PWM Sabitleri

### PWM Frekansı Hesabı

```
APB2 timer saati = 96 MHz (SYSCLK=96 MHz, APB2 prescaler=1, timer x1)
TIM1 ön bölücü = 0 → TIM1 tick = 96 MHz

PWM_PERIOD_COUNTS = 3199
PWM frekansı = 96 MHz / (3199 + 1) = 96 MHz / 3200 = 30.0 kHz

Duty çözünürlüğü: 0..3199, yaklaşık 11.6 bit
```

**`PWM_PERIOD_COUNTS`** değiştirilerek PWM frekansı ayarlanabilir:
- 20 kHz istiyorsanız: `96 MHz / 20000 - 1 = 4799`
- 40 kHz istiyorsanız: `96 MHz / 40000 - 1 = 2399`

### Deadtime Hesabı

```
DEADTIME_COUNTS = 50
TIM1 tick süresi = 1 / 96 MHz ≈ 10.4 ns
MCU deadtime = 50 × 10.4 ns ≈ 521 ns

L6388 dahili propagasyon gecikmesi ≈ 300-400 ns
Toplam ≈ 820-920 ns

[AYAR] Osiloskopla ölçüldükten sonra en küçük güvenli değere indirilebilir.
        MOSFET Qg ve gate direncine bağlı olarak değişir.
```

Deadtime nasıl ayarlanır:
- `DEADTIME_COUNTS` artırılırsa toplam deadtime artar → daha güvenli ama verimlilik düşer
- Osiloskopla PA8 ve PA7'de eş zamanlı HIGH görünüyorsa artırılmalıdır
- `DTG[7:0]` bit7=0: deadtime = DTG × (1 / TIM1_clock)

### TIM3 Kontrol Zamanlayıcısı

```
APB1 timer saati = 96 MHz (APB1=48 MHz, prescaler=2 → x2 = 96 MHz)
CTRL_TIMER_PRESCALER = 95 → tick = 96 MHz / 96 = 1 MHz (1 µs)
CTRL_TIMER_PERIOD = 79 → ISR = 1 MHz / 80 = 12.5 kHz (80 µs)
```

ISR frekansını değiştirmek için `CTRL_TIMER_PERIOD` ayarlanır:
- 10 kHz: period = `1 MHz / 10000 - 1 = 99`
- 20 kHz: period = `1 MHz / 20000 - 1 = 49`

**Not:** ISR frekansı artarsa ADC yükü oransal artar (`ADC_DECIMATION` de ayarlanmalı).

---

## Hall Sensör Sabitleri

| Sabit | Değer | Açıklama |
|---|---|---|
| `HALL_OVERSAMPLE` | `7` | Tek okumada kaç kez GPIO okunur |
| `MIN_STATE_INTERVAL_US` | `40` | Debounce: durum değişimleri arası minimum süre |
| `INVALID_HALL_HOLD_US` | `1500` | Geçersiz hall'da son geçerli durumu tut bu kadar |
| `HALL_PROFILE_COUNT` | `4` | Farklı hall eşleştirme profili sayısı |

**`HALL_OVERSAMPLE`** artırılırsa gürültü azalır ama ISR süresi uzar. 7 değer için 21 GPIO okuma ≈ ek yük.

**`MIN_STATE_INTERVAL_US`** çok küçük yapılırsa gürültü kaynaklı sahte geçişler kabul edilir. Çok büyük yapılırsa yüksek hız çalışmada doğru geçişler reddedilir.

**`INVALID_HALL_HOLD_US`** aşılırsa motor durdurulur. Kısa kesilir → hassas ama düşme riski. Uzun tutulur → hata geç yakalanır. 

---

## ADC Sabitleri

| Sabit | Değer | Açıklama |
|---|---|---|
| `ADC_VREF` | `3.3f` | ADC referans voltajı |
| `ADC_MAX_COUNTS` | `4095.0f` | 12-bit maksimum |
| `SHUNT_OHMS` | `0.0005f` | Shunt direnci (0.5 mΩ) [TASARIM] |
| `INA_GAIN_DEFAULT` | `20.0f` | INA181A1QDBVRQ1 — A1 varyantı, 20 V/V gain [TASARIM] |
| `ADC_DECIMATION` | `4` | Kaçıncı ISR tick'inde ADC örneklenir |
| `CURRENT_FILTER_ALPHA` | `0.20f` | EMA düşük geçiren katsayı |
| `ADC_CALIBRATION_SAMPLES` | `128` | `zeroi` kaç örnek alır |

**`CURRENT_FILTER_ALPHA`** düşürülürse filtre daha yavaş tepki verir (daha düzgün ama geç). Artırılırsa hızlı tepki ama gürültülü. 0.20 başlangıç için makul.

**`ADC_DECIMATION`** ile gerçek ADC örnekleme frekansı:
- `CTRL_TICK_HZ / ADC_DECIMATION = 12500 / 4 = 3125 Hz`

---

## Duty ve Rampa Sabitleri

| Sabit | Değer | Açıklama |
|---|---|---|
| `DUTY_DEFAULT` | `70` | Başlangıç komut duty (0..255) |
| `DUTY_MIN_ACTIVE` | `8` | Etkin sürüş için minimum duty |
| `DUTY_RAMP_UP_STEP` | `2` | ISR tick başına artış adımı |
| `DUTY_RAMP_DOWN_STEP` | `4` | ISR tick başına azalış adımı |

**`DUTY_TO_PWM(d)`** makrosu:
```c
duty_pwm = d * PWM_PERIOD_COUNTS / 255
// Örnek: d=50 → 50 × 3199 / 255 = 627 (PWM CCR değeri)
```

**`DUTY_MIN_ACTIVE`**: Çok küçük duty'de MOSFET'ler tam açılıp kapanmadan önce deadtime süresi geçebilir. 8/255 ≈ %3.1 başlangıç için güvenli.

**Rampa hızı hesabı** (0'dan hedefe):
```
DUTY_RAMP_UP_STEP = 2, ISR = 12.5 kHz
255 step / 2 per tick = 127.5 tick = 127.5 / 12500 = ~10.2 ms rampa süresi
```

---

## Koruma Sabitleri

| Sabit | Değer | Açıklama |
|---|---|---|
| `CURRENT_SOFT_LIMIT` | `450` | ADC delta soft limit eşiği |
| `CURRENT_HARD_LIMIT` | `700` | ADC delta hard limit eşiği |
| `HARD_LIMIT_STRIKES` | `3` | Hard limit fault için ardışık aşım sayısı |
| `SOFT_BACKOFF_MIN` | `3` | Soft backoff minimum miktarı |
| `SOFT_BACKOFF_DIVISOR` | `16` | Backoff hesaplama böleni |
| `SOFT_BACKOFF_MAX` | `80` | Soft backoff maksimum miktarı |
| `FAULT_REASON_MAX` | `48` | Fault mesajı maksimum karakter sayısı |

**Soft limit backoff formülü:**
```
over = currentDelta - CURRENT_SOFT_LIMIT
backoff = SOFT_BACKOFF_MIN + (over / SOFT_BACKOFF_DIVISOR)
backoff = min(backoff, SOFT_BACKOFF_MAX)
```

**Limitleri kalibre etmek için:**
1. Motor ayakta boşta çalışırken `current` komutu — `dlt` değerini not et
2. Tam yükte `dlt` değerini not et
3. `CURRENT_SOFT_LIMIT` = boşta × 1.5 ile başla
4. `CURRENT_HARD_LIMIT` = maksimum kabul edilebilir × 0.9

**Not:** ADC delta, INA181 kazancından bağımsızdır — kalibre sayılar kullanmak güvenlidir.

---

## CLI Sabitleri

| Sabit | Değer | Açıklama |
|---|---|---|
| `CLI_BAUD` | `115200` | USART2 baud hızı |
| `CLI_LINE_BUF` | `96` | Satır tamponu karakter sayısı |
| `CLI_IDLE_PARSE_MS` | `120` | Newline gelmeden otomatik parse süresi |
| `CLI_UART_HANDLE` | `huart2` | Kullanılan UART handle |
