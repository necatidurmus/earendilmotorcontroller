# F411 Motor Controller + F446 Bridge Test System

STM32F411 tabanlı BLDC motor controller firmware ve F446 tabanlı test bridge sistemi.

## Proje Yapısı

```
.
├── f411-motor-cube/          # F411 motor controller firmware (STM32Cube)
│   ├── App/                  # Uygulama kodu (motor kontrol, Hall, UART)
│   ├── Core/                 # CubeMX generated kod (GPIO, TIM, USART)
│   ├── Drivers/              # HAL/LL drivers (framework tarafından sağlanır)
│   ├── f411-motor-cube.ioc   # CubeMX konfigürasyon dosyası
│   └── platformio.ini        # PlatformIO build config
│
├── f446-bridge-test/         # F446 test bridge firmware (Arduino)
│   ├── src/main.cpp          # Bridge ana kod
│   ├── include/              # Config header
│   └── platformio.ini        # PlatformIO build config
│
├── tools/                    # PC test araçları
│   ├── f446_motor_gui.py     # Tkinter GUI (motor kontrol, telemetry)
│   └── f446_serial_smoke_test.py  # Otomatik smoke test
│
└── README.md                 # Bu dosya
```

## F411 Motor Controller

STM32F411CEU6 tabanlı BLDC hub motor controller.

### Özellikler

- **PWM:** TIM1 edge-aligned 20 kHz (ARR=4799)
- **Commutation:** 6-step, Hall sensor feedback
- **Hall Capture:** EXTI on PB6/PB7/PB8, TIM2 1 MHz timestamps
- **UART:** DMA RX circular + DMA TX ring buffer (115200 baud)
- **Speed PI:** Closed-loop RPM control with command heartbeat
- **Fault System:** Latching faults, requires `clrerr` to clear

### Pin Mapping

| Pin | Function |
|-----|----------|
| PA8, PA7 | Phase A high/low (TIM1_CH1/CH1N) |
| PA9, PB0 | Phase B high/low (TIM1_CH2/CH2N) |
| PA10, PB1 | Phase C high/low (TIM1_CH3/CH3N) |
| PB6, PB7, PB8 | Hall sensors (EXTI, pull-up) |
| PA2, PA3 | UART TX/RX (115200 baud) |
| PC13 | LED |

### Build & Flash

```bash
cd f411-motor-cube
pio run                    # Build
pio run -t upload          # Flash via ST-Link
pio device monitor         # Serial monitor (115200 baud)
```

### UART Protocol

**Commands:**
- `mode duty` / `mode speed` - Çalışma modu
- `f<pwm>` / `b<pwm>` - Forward/backward (duty 0-250)
- `rpm <signed>` - RPM hedef (speed mode, ±500)
- `stop` - Coast (all gates off)
- `brake` - Coast (active brake disabled)
- `hall` / `status` - Diagnostics
- `clrerr` - Clear fault latch
- `identify` - Hall map auto-detect
- `pi <kp> <ki>` - Speed PI gains
- `base <lo> <mid> <hi>` - Feed-forward PWM
- `boost <lo> <mid> <hi> <ms>` - Start boost
- `ramp <up> <down>` - RPM ramp rates

**Telemetry (compact):**
```
RPM:<measured>,T:<target>,D:<duty>,DIR:<F|R|N>,PH:<phase>,SP:<0|1>,BRAKE:<0|1>,FC:<fault>,H:<hall>,PWM_SET:<targetDuty>,PWM_ACT:<actualDuty>
```

### Safety Features

- **Command Watchdog:** 800 ms timeout, motor stops if no command
- **Host Disconnect:** 2000 ms timeout, raises FAULT_HOST_LOST
- **Fault Latching:** Motor stops on fault, requires `clrerr`
- **No Current Sense:** Use current-limited PSU for testing
- **No Active Brake:** `brake` command = coast (safety)

## F446 Bridge Test System

STM32F446RE tabanlı UART bridge, F411 motor controller'ı PC'den test etmek için.

### Mimari

```
PC (GUI/CLI)
    │ USB Serial (ST-Link VCP)
    │ 115200 baud
    ▼
F446 Bridge (Nucleo-F446RE)
    │ USART1 (PA9/PA10)
    │ 115200 baud
    ▼
F411 Motor Controller (USART2)
```

### Özellikler

- **Single Motor:** M1 prefix ile telemetry forwarding
- **Command Passthrough:** F411 komutlarını doğrudan iletir
- **Bridge Commands:** `ping`, `help`, `bridge on/off`, `stop`, `safe`
- **LED Heartbeat:** 500 ms blink

### Build & Flash

```bash
cd f446-bridge-test
pio run                    # Build
pio run -t upload          # Flash via ST-Link
pio device monitor         # Serial monitor (115200 baud)
```

### Bridge Commands

```
ping                 → pong
help                 → Komut listesi
bridge on/off        → Telemetry forwarding aç/kapat
m1 <cmd>             → F411'e komut gönder
raw <cmd>            → F411'e raw komut gönder
stop                 → Safe stop (rpm 0 + stop)
safe / estop         → Safe stop (rpm 0 + stop)
```

**Direct passthrough examples:**
```
f50, b50, rpm 30, rpm -30, mode duty, mode speed, hall, status
```

## Test Tools

### f446_motor_gui.py

Tkinter tabanlı GUI, motor kontrol ve telemetry görüntüleme.

**Features:**
- PWM/RPM slider kontrolü
- Forward/backward/stop butonları
- Heartbeat (300 ms default)
- Telemetry parsing (RPM, duty, fault, hall)
- Raw command gönderme

**Usage:**
```bash
pip install pyserial
python tools/f446_motor_gui.py
```

### f446_serial_smoke_test.py

Otomatik smoke test, motor hareket ettirmeden bağlantı testi.

**Tests:**
- Ping/pong
- Help command
- Bridge on/off
- M1 status/hall (F411 bağlıysa)
- Stop command
- Unknown command handling

**Usage:**
```bash
python tools/f446_serial_smoke_test.py --port /dev/ttyACM0
```

## Bring-up Sequence

### 1. F411 Firmware Test

```bash
cd f411-motor-cube
pio run -t upload
pio device monitor
```

**Verify:**
```
> status
--- STATUS ---
Mode: DUTY
Phase: STOPPED
...

> hall
[INFO] Hall=5 State=0

> help
=============================
 f/forward  |  f<0-255>
 b/backward |  b<0-255>
 ...
```

### 2. F446 Bridge Test

```bash
cd f446-bridge-test
pio run -t upload
pio device monitor
```

**Verify:**
```
> ping
pong

> help
F446 bridge commands:
  ping                 -> pong
  ...

> m1 status
TX|status
M1|--- STATUS ---
...
```

### 3. Motor Test (GUI)

```bash
python tools/f446_motor_gui.py
```

1. Connect to F446 (ST-Link VCP port)
2. Click "mode duty"
3. Click "kick off" (safety)
4. Set PWM = 10
5. Click "Forward"
6. Watch telemetry (RPM, duty, hall)
7. Click "STOP"

### 4. Smoke Test

```bash
python tools/f446_serial_smoke_test.py --port /dev/ttyACM0
```

**Expected output:**
```
PASS  ping -> pong
PASS  help -> command list
PASS  bridge on -> OK
PASS  m1 status -> response
PASS  stop -> OK
PASS  unknown -> ERR
```

## Hardware Connections

### F446 → F411

```
F446 (Nucleo-F446RE)        F411 (Blackpill)
─────────────────────        ────────────────
PA9  (USART1_TX)    →       PA3 (USART2_RX)
PA10 (USART1_RX)    ←       PA2 (USART2_TX)
GND                 ↔       GND
```

### Power

- F446: USB powered (ST-Link)
- F411: External 12V PSU (current-limited, 0.5A max)
- **Common ground required**

## Safety Rules

1. **Motor disconnected** for first TIM1 scope test
2. **Current-limited PSU** (0.3-0.5A) for all motor tests
3. **Kick disabled** by default (`kick off`)
4. **No active brake** during bring-up
5. **Fault latch** requires `clrerr` before new motion
6. **Command heartbeat** required (800 ms timeout)

## Known Limitations

- **No current sense:** No ADC, no current limiting
- **No active brake:** `brake` = coast only
- **Storage disabled:** `save` commands return error (flash unsafe)
- **Dead-time unverified:** DTG=63 (~0.66 µs), needs scope check
- **Hall map:** Default map may need `identify` for your motor

## Troubleshooting

### F411 doesn't respond

- Check UART wiring (PA2/PA3)
- Verify baud rate (115200)
- Check F411 power (12V + GND)
- Try `m1 ping` from F446

### Motor doesn't spin

- Send `clrerr` (fault may be latched)
- Send `kick off` (safety default)
- Check Hall sensors (send `hall`)
- Verify mode (`mode duty` or `mode speed`)
- Check PSU current limit

### Telemetry missing

- Send `bridge on` (F446)
- Check F411 telemetry interval (`telper 100`)
- Verify F446 → F411 UART connection

## License

Proprietary - Internal use only

## Contact

For questions or issues, contact the firmware team.
