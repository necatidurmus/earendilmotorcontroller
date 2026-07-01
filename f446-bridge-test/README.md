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
| `stop` | Normal durdurma: `rpm 0` + `stop` (fault latch yok) |
| `safe` / `alloff` | Coast durdurma: `safe` + `stop` (motor coast, no fault latch) |
| `estop` | Acil durdurma: F411'e `estop` gönderir; sonraki hareket komutu fault'u temizleyebilir |
| `all <cmd>` | Tüm motorlara gönderir (tek motor build'de M1'e gider) |

### Doğrudan F411 Passthrough (unlock gerektirmez)

Aşağıdaki komutlar `m1` prefix'i olmadan doğrudan F411'e forward edilir
ve bridge service-lock gerektirmez:

```text
f<n>, b<n>
stop, s
x / brake (aktif fren; current-limited PSU ile)
rpm <signed>, pwm <n>
mode duty / speed, mode normal / control, pid on / off
hall, status, spstat, help, clrerr
debug on/off, dbg on/off
telper <ms>
kick on / off
ramp on / off
map, map validate, map candidate, map default
mapreset, reload
```

Not: `telper` sadece telemetry interval'ını değiştirir; motor sürüş,
PI, PWM, Flash veya gate davranışını etkilemez. Bu nedenle
`telper <n>` ve `m1 telper <n>` service-lock olmadan geçer.

### Service Komutları (bridge unlock_service + F411 arming gerektirir)

Aşağıdaki komutlar bridge `unlock_service CURRENT_LIMITED_BENCH_SUPPLY`
ve gerekli olanlarda ek olarak F411 `arm` gerektirir:

```text
m1 pi <kp> <ki>              # PI gains
m1 kp <v> / m1 ki <v>        # PI gain tek başına
m1 base <b1>..<b8>           # PI base 8-band
m1 boost <b1>..<b8> <ms>     # PI boost 8-band + ms
m1 ramp <up_rpm_s> <down_rpm_s>  # speed ramp
m1 kickduty <n>              # startup kick duty
m1 kickms <n>                # startup kick süresi
m1 ramprate <n>              # duty ramp step
m1 rampms <n>                # duty ramp interval
m1 defpwm <n>                # bare f/b default duty
m1 brake                     # aktif fren
m1 savecfg / m1 save / m1 saveall  # config Flash'a yaz
m1 loadcfg                   # config Flash'tan yükle
m1 erasecfg                  # config Flash kayıtlarını sil
m1 defaults                  # runtime defaults'a dön
m1 map set / map apply / map discard / map reset
m1 map save / map load / map edit
m1 identify                  # (F411 arm service gerekli)
m1 scan                      # (F411 arm service gerekli)
m1 test                      # (F411 arm service gerekli)
m1 gatetest <s> <duty>       # (F411 arm gatetest gerekli)
```

`m1 savecfg` motor config'i Flash'a yazar (PI, base, boost, ramp,
kick, default_pwm, brake_hold_ms, telper). Hall map **ayrıdır**;
Hall map için `m1 map save` kullanılır.

PI bant formatı: `base` komutu 8 PWM değeri; `boost` komutu 8 PWM
değeri ve tüm bantlar için ortak tek `ms` değeri alır.

## Telemetry Bridge Formatı

F411'den gelen her satır PC'ye şu prefix ile gönderilir:

```text
M1|RPM:23,T:30,D:67,DIR:F,APP_PH:1,SP:1,BRAKE:0,FC:0,H:5,PWM_SET:67,PWM_ACT:67,QDROP:0
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

### Gate testi (osiloskop, motor bağlı değil)

```text
bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY
m1 arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND
m1 gatetest 0 10
```

### Identify (Hall map çıkarma)

```text
bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY
m1 arm service CURRENT_LIMITED_BENCH_SUPPLY
m1 identify
```

### Speed mode test

```text
m1 mode speed
m1 rpm 30
stop
```

## Boot Mesajları

F446 başladığında şu mesajları göndermelidir:

```text
BOOT|F446 single-motor bridge ready
BOOT|PC_BAUD=115200
BOOT|MOTOR_BAUD=115200
```

## Dosya Yapısı

```text
f446-bridge-test/
    platformio.ini
    include/f446_bridge_config.h
    src/main.cpp
    README.md
tools/
    f446_motor_gui.py
    f446_serial_smoke_test.py
```

## Güvenlik

- F446 reset atınca F411'e otomatik motor komutu göndermez.
- GUI bağlanınca motor otomatik hareket etmez.
- `stop` normal durdurma: `rpm 0` + kısa bekleme + `stop`. Fault latch oluşturmaz.
- `safe` / `alloff` coast durdurma: `safe` + kısa bekleme + `stop`. Motor coast olur, fault latch yok.
- `estop` acil durdurma: F411'e gerçek `estop` gönderir. Güncel F411 politikasında sonraki hareket komutu fault'u temizleyip yeniden başlayabilir; `clrerr` manuel temizleme için kullanılabilir.
- `x` / `brake` aktif fren uygular (tüm low-side MOSFET'ler ON). Akım algılama yoktur; yalnızca düşük hızda ve akım limitli PSU ile kullanın.
- F411'in kendi command watchdog'ı (800 ms) heartbeat kesilirse motoru durdurur.
- GUI kapanırken safe stop gönderir.
- Donanımda akım ölçümü yoktur. Akım limitli PSU kullanın.

## Önemli Notlar

- `rpm stop` **kullanmayın**. Doğru komut `rpm 0`'dır.
- GUI duty hareketlerinde `f<pwm>` / `b<pwm>` kullanır (bare `f`/`b` değil).
- F446 sadece bridge katmanıdır. Motor komütasyonu F411'de çalışır.

## GUI ve Smoke Test

```bash
pip install pyserial
python tools/f446_motor_gui.py
python tools/f446_serial_smoke_test.py --port /dev/ttyACM0
```

## Çok Motorlu Genişleme

Kod `MotorPort` struct'ı ile tasarlanmıştır. `MOTOR_COUNT` artırılarak ve ek UART portları eklenerek M2/M3/M4 desteği eklenebilir. Şu an sadece M1 aktiftir.
