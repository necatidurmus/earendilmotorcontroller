# Pin Haritası

## STM32F411 Black Pill — Earendil Motor Kontrolcüsü

### Motor Güç Aşaması

| MCU Pin | TIM1 Kanal | L6388 Bağlantısı | MOSFET | Açıklama |
|---|---|---|---|---|
| PA8 | CH1 (AF1) | L6388_A INH | AH | Yüksek taraf A |
| PA7 | CH1N (AF1) | L6388_A INL | AL | Düşük taraf A |
| PA9 | CH2 (AF1) | L6388_B INH | BH | Yüksek taraf B |
| PB0 | CH2N (AF1) | L6388_B INL | BL | Düşük taraf B |
| PA10 | CH3 (AF1) | L6388_C INH | CH | Yüksek taraf C |
| PB1 | CH3N (AF1) | L6388_C INL | CL | Düşük taraf C |

> **Önemli:** PA7/PB0/PB1 artık GPIO output DEĞİL. TIM1 complementary output olarak AF1 modunda çalışır.

### Hall Sensörleri

| MCU Pin | Sensör | Pull | Açıklama |
|---|---|---|---|
| PB6 | Hall A | Dahili pull-up | Faz A hall |
| PB7 | Hall B | Dahili pull-up | Faz B hall |
| PB8 | Hall C | Dahili pull-up | Faz C hall |

### Analog Girişler

| MCU Pin | ADC Kanal | Bağlantı | Açıklama |
|---|---|---|---|
| PA0 | ADC1_IN0 | INA181 çıkışı | Akım ölçümü (ISENSE) |
| PA4 | ADC1_IN4 | Voltaj bölücü | Voltaj ölçümü (VSENSE) |

### UART (CLI)

| MCU Pin | Fonksiyon | Not |
|---|---|---|
| PA2 | USART2_TX (AF7) | PC'ye →  |
| PA3 | USART2_RX (AF7) | PC'den ← |

> **Neden USART2?** PA9/PA10 hem TIM1_CH2/CH3 (AF1) hem USART1_TX/RX (AF7) olarak kullanılabilir. Motor PWM için AF1 seçildi → USART1 kullanılamaz → USART2 PA2/PA3'e taşındı.

### Diğer

| MCU Pin | Fonksiyon | Açıklama |
|---|---|---|
| PC13 | LED | Aktif düşük (Black Pill özelliği) |

### Kullanılmayan Pin'ler (gelecek için)

| MCU Pin | Potansiyel Kullanım |
|---|---|
| PA1 | TIM2_CH2 veya ekstra ADC |
| PA5 | SPI1_SCK veya ekstra GPIO |
| PB12 | TIM1_BKIN (harici overcurrent trip) |

### USB CDC (Alternatif CLI Taşıması)

| MCU Pin | Fonksiyon | Not |
|---|---|---|
| PA11 | OTG_FS_DM (D-) | USB Full-Speed |
| PA12 | OTG_FS_DP (D+) | USB Full-Speed |

`motor_config.h` içinde `CLI_TRANSPORT` seçimi:
- `CLI_TRANSPORT_UART` (varsayılan): USART2 PA2/PA3, 115200 baud
- `CLI_TRANSPORT_CDC`: USB CDC, PA11/PA12 üzerinden sanal seri port

CDC seçildiğinde de SYSCLK 96 MHz'dir (USB 48 MHz PLLQ gereksinimi). Tüm timer hesapları bu saate göre yapılır.
