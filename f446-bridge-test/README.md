# F446 Bridge Test Firmware

STM32F446 tabanlı bridge/test firmware. H7 yerine F446 kullanarak F411 BLDC motor controller'ı test etmek için tasarlanmıştır.

## Mimari

```text
PC GUI / CLI smoke test
        │ USB Serial (ST-Link VCP / CDC), 115200 baud
        ▼
STM32F446 bridge firmware
        │ USART1, 115200 baud
        ▼
F411 motor controller (USART2)
```

İlk sürüm **tek F411 motor controller** (M1) için tasarlanmıştır. Kod mimarisi ileride 4 motorlu (FL/RL/FR/RR) yapıya genişletilebilir.

## Varsayılan Pinler (Nucleo-F446RE)

```text
F446 USART1 TX = PA9   -> F411 USART2 RX = PA3
F446 USART1 RX = PA10  <- F411 USART2 TX = PA2
GND ortak
Baudrate = 115200
```

F446 PC bağlantısı `Serial` (ST-Link VCP) üzerinden yapılır.

## Build ve Upload

```bash
cd f446-bridge-test
pio run
pio run -t upload
pio device monitor -b 115200
```

## F446 Bridge Komutları

F446 USB serial üzerinden şu komutları kabul eder:

| Komut | Açıklama |
|-------|----------|
| `ping` | `pong` cevabı döner |
| `help` | Komut listesini gösterir |
| `bridge on` | F411 telemetry forward açılır |
| `bridge off` | F411 telemetry forward kapatılır |
| `m1 <cmd>` | `<cmd>` satırını F411'e gönderir |
| `raw <cmd>` | `<cmd>` satırını F411'e gönderir |
| `stop` | Güvenli stop: `rpm 0` + `stop` |
| `safe` | Güvenli stop: `rpm 0` + `stop` |
| `estop` | Güvenli stop: `rpm 0` + `stop` |
| `all <cmd>` | Tüm motorlara gönderir (tek motor build'de M1'e gider) |

### Doğrudan F411 Passthrough

Aşağıdaki komutlar doğrudan F411'e forward edilir:

```text
f50, b50, stop, rpm 30, rpm -30, rpm 0, pwm 50,
mode duty, mode speed, hall, status, clrerr,
debug on/off, telper <n>, kick on/off, kickduty <n>,
kickms <n>, ramp on/off, ramprate <n>, rampms <n>,
defpwm <n>, gatetest <sector> <duty>, scan, test,
identify, map, mapreset, reload, spstat,
base <lo> <mid> <hi>, boost <lo> <mid> <hi> <ms>,
pi <kp> <ki>, kp <v>, ki <v>
```

## Telemetry Bridge Formatı

F411'den gelen her satır PC'ye şu prefix ile gönderilir:

```text
M1|RPM:23,T:30,D:67,DIR:F,PH:1,SP:1,BRAKE:0,FC:0,H:5,PWM_SET:67,PWM_ACT:67
```

F446 kendi mesajları için şu prefix'leri kullanır:

```text
BOOT|...    (başlangıç mesajları)
OK|...      (başarılı işlem)
ERR|...     (hata)
WARN|...    (uyarı)
TX|...      (F411'e gönderilen komut)
```

## Güvenli İlk Test

### Motor bağlı değilken

```text
ping
m1 help
m1 status
m1 hall
```

Gate sinyallerini osiloskopla doğrulayın.

### Düşük duty test (akım limitli PSU kullanın)

```text
m1 mode duty
m1 kick off
m1 ramp on
m1 f10
stop
m1 b10
stop
```

### Speed mode test

```text
m1 mode speed
m1 rpm 30
stop
```

## Önemli Notlar

- `rpm stop` **kullanmayın**. Doğru komut `rpm 0`'dır.
- GUI duty hareketlerinde `f<pwm>` / `b<pwm>` kullanır (bare `f`/`b` değil).
- Donanımda akım ölçümü yoktur. İlk testler akım limitli güç kaynağı ile yapılmalıdır.
- F446 sadece bridge katmanıdır. Motor komütasyonu F411'de çalışır.
- F446 reset atınca F411'e otomatik motor komutu göndermez.

## GUI ve Smoke Test

```bash
pip install pyserial
python tools/f446_motor_gui.py
python tools/f446_serial_smoke_test.py --port /dev/ttyACM0
```

## Çok Motorlu Genişleme

Kod `MotorPort` struct'ı ile tasarlanmıştır. `MOTOR_COUNT` artırılarak ve ek UART portları eklenerek M2/M3/M4 desteği eklenebilir. Şu an sadece M1 aktiftir.
