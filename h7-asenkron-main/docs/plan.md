# Yeni Motor Ekleme Rehberi - H7 Asenkron Motor Sistemi

## Mevcut MOTOR FL (PD5/PD6) Nasıl Çalışıyor?

MOTOR FL şu zincirle çalışıyor:

```
Terminal Girdisi → CommandParser → MotionController → MotorDispatcher → Serial2 (USART2) → PD5(TX)/PD6(RX) → F411 Motor Kartı
```

### 1. Dosya Bazlı Haritalama

| Dosya | Satır | Ne Yapılır |
|-------|-------|------------|
| `types.h` | 9-15 | `MotorId` enum'ına motor ID eklenir |
| `config.h` | 19-24 | `MOTOR_UART_XX` makrosu tanımlanır |
| `config.h` | 27-30 | PWM_MIN / PWM_MAX zaten global |
| `main.cpp` | 36-37 | `setRx()` / `setTx()` ile pin atanır |
| `main.cpp` | 62 | Başlangıç mesajına pin bilgisi eklenir |
| `main.cpp` | ~40 | `dispatcher.begin()` UART'ı başlatır |
| `motor_dispatcher.cpp` | 4-13 | `_uartMap[]` dizisine UART pointer eklenir |
| `motion_controller.cpp` | 3-48 | Yön hesaplamasına motor dahil edilir |
| `platformio.ini` | 18-22 | `ENABLE_HWSERIALX` build flag'i eklenir |

### 2. MOTOR FL Tam Akışı

```
Kullanıcı "forward 200" yazar
  → CommandParser: {type=CMD_FORWARD, pwm=200, valid=true}
  → MotionController::compute(): 4 motor için MotorCommand üretir
     MOTOR_FL için: {motorId=0, direction=DIR_FORWARD, pwm=200}
  → MotorDispatcher::sendMotorCommand():
     _uartMap[MOTOR_FL] → &Serial2
     Serial2.write("forward 200\r\n")
  → USART2 TX → PD5 pini → F411 motor kartına metin gönderilir
  → F411 kartı gerçek PWM'i üretir (H7 PWM üretmez!)
```

**Önemli:** H7 asla donanımsal PWM üretmez. PWM değeri (0-255) sadece UART üzerinden F411'e parametre olarak gönderilir.

---

## Keşfedilen Önemli Noktalar

- **Serial6 çakışması:** `Serial6` (USART6) halihazırda `FTDI_UART` olarak `config.h:16` satırında kullanılıyor (PG9/PG14). Yeni motor için Serial6 kullanılamaz; farklı bir UART peripheral'ı seçilmelidir.
- **PD8/PD9 çakışması:** MOTOR_RL (USART3) ve ST-LINK VCP aynı pinleri kullanıyor — potansiyel conflict.
- `Serial2` → `USART2`, `ENABLE_HWSERIAL2` build flag'i ile aktifleşir. ilgili flag olmadan `extern HardwareSerial SerialX` derlenmez.
- Pin atamaları `main.cpp`'de `setRx()`/`setTx()` ile `dispatcher.begin()` çağrısından **önce** yapılmalıdır.
- `MOTOR_COUNT` enum değerini artırmak `_uartMap[]`, `_statusMap[]`, `outMotors[]` gibi tüm dizileri otomatik genişletir.

---

## Mevcut Pin Kullanımı

| Pin Çifti | Kullanım | UART | Not |
|-----------|----------|------|-----|
| PD5/PD6 | MOTOR_FL | USART2 | |
| PD8/PD9 | MOTOR_RL | USART3 | ST-LINK VCP ile çakışma riski |
| PD0/PD1 | MOTOR_FR | UART4 | |
| PC12/PD2 | MOTOR_RR | UART5 | |
| PG9/PG14 | FTDI_UART | USART6 | Debug portu — yeni motor için kullanılamaz |

---

## Yeni Motor Eklemek İçin Adım Adım Rehber

Aşağıdaki örnekte **MOTOR_XX** (örneğin 5. bir motor) nasıl eklenir gösterilmiştir. Proje şu anda 4 motor tanımlı (FL, RL, FR, RR). Bu şablonu takip ederek 5., 6. vs. motor ekleyebilirsiniz.

### Adım 1: `platformio.ini` — Build Flag Ekle

Kullanılacak USART peripheral'ına karşılık gelen `ENABLE_HWSERIALX` flag'ini ekleyin.

```ini
build_flags =
    ; ... mevcut flagler ...
    -DENABLE_HWSERIAL7    ; Yeni motor için (UART7 kullanılacaksa)
```

**Hangi Serial hangi USART'a denk gelir?**

| Macro | USART | STM32duino Nesnesi | Mevcut Durum |
|-------|-------|-------------------|-------------|
| `ENABLE_HWSERIAL2` | USART2 | Serial2 | MOTOR_FL |
| `ENABLE_HWSERIAL3` | USART3 | Serial3 | MOTOR_RL |
| `ENABLE_HWSERIAL4` | UART4 | Serial4 | MOTOR_FR |
| `ENABLE_HWSERIAL5` | UART5 | Serial5 | MOTOR_RR |
| `ENABLE_HWSERIAL6` | USART6 | Serial6 | FTDI_UART (KULLANILAMAZ) |
| `ENABLE_HWSERIAL7` | UART7 | Serial7 | Boşta ✓ |
| `ENABLE_HWSERIAL8` | USART8 | Serial8 | Boşta ✓ |

**UYARI:** Serial6 (USART6) halen FTDI_UART tarafından kullanılmaktadır. Yeni motor için Serial7 veya Serial8 seçilmelidir.

STM32H723 pin tablosundan uygun TX/RX pinlerini belirleyin (AF mapping doğru olmalı).

### Adım 2: `types.h` — MotorId Enum'ına Ekle

```cpp
enum MotorId : uint8_t {
    MOTOR_FL = 0,
    MOTOR_RL = 1,
    MOTOR_FR = 2,
    MOTOR_RR = 3,
    MOTOR_XX = 4,      // ← YENİ
    MOTOR_COUNT = 5     // ← 4→5 güncelle
};
```

### Adım 3: `config.h` — UART Makrosu Ekle

```cpp
#define MOTOR_UART_FL       Serial2
#define MOTOR_UART_RL       Serial3
#define MOTOR_UART_FR       Serial4
#define MOTOR_UART_RR       Serial5
#define MOTOR_UART_XX       Serial7   // ← YENİ (UART7 örneği)
#define MOTOR_UART_BAUD     115200
```

Ayrıca `motorName()` fonksiyonuna yeni motoru ekleyin:

```cpp
inline const char* motorName(uint8_t id) {
    switch (id) {
        case MOTOR_FL: return "FL";
        case MOTOR_RL: return "RL";
        case MOTOR_FR: return "FR";
        case MOTOR_RR: return "RR";
        case MOTOR_XX: return "XX";   // ← YENİ
        default:       return "??";
    }
}
```

### Adım 4: `main.cpp` — Pin Atama ve Başlatma

`setup()` fonksiyonuna, `dispatcher.begin()` çağrısından ÖNCE pin atamalarını ekleyin:

```cpp
// Mevcut pin atamaları
MOTOR_UART_FL.setRx(PD6);
MOTOR_UART_FL.setTx(PD5);
// ...
// Yeni motor pin ataması
MOTOR_UART_XX.setRx(PE7);    // UART7 RX için örnek pin — datasheet'e bakın!
MOTOR_UART_XX.setTx(PE8);    // UART7 TX için örnek pin — datasheet'e bakın!
```

Başlangıç mesajına da ekleyin:

```cpp
terminal.println("MOTOR XX   : Serial7  RX=PE7  TX=PE8");
```

Başlık satırını güncelleyin:

```cpp
terminal.println("  5x UART -> 5x STM32F411 Motor Control");
```

### Adım 5: `motor_dispatcher.cpp` — UART Haritasına Ekle

```cpp
void MotorDispatcher::begin() {
    _uartMap[MOTOR_FL] = &MOTOR_UART_FL;
    _uartMap[MOTOR_RL] = &MOTOR_UART_RL;
    _uartMap[MOTOR_FR] = &MOTOR_UART_FR;
    _uartMap[MOTOR_RR] = &MOTOR_UART_RR;
    _uartMap[MOTOR_XX] = &MOTOR_UART_XX;   // ← YENİ

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        _uartMap[i]->begin(MOTOR_UART_BAUD);
    }
}
```

### Adım 6: `motion_controller.cpp` — Yön Hesaplamasına Dahil Et

`compute()` fonksiyonunda `MOTOR_XX` için yön/pwm atamasını yapın. Örneğin:

```cpp
case CMD_FORWARD:
    setAll(outMotors, DIR_FORWARD, (uint8_t)cmd.pwm);
    // setAll zaten MOTOR_COUNT kadar döngü yapar, MOTOR_XX de dahil olur
    break;
```

Eğer motor özel bir davranış göstermeliyse, ayrı satırda atama yapın:

```cpp
outMotors[MOTOR_XX] = { MOTOR_XX, DIR_BACKWARD, (uint8_t)cmd.pwm };
```

### Adım 7: `status_manager.cpp` — Durum Takibi

`_uartMap` ve `_statusMap` gibi yapılar `MOTOR_COUNT` ile boyutlandırıldığından, enum değerinin güncellenmesi yeterlidir. Ekstra kod gerekmez — ama kontrol edin.

---

## Pin Seçimi Kontrol Listesi

Yeni motor eklemeden önce şu kontrolleri yapın:

1. **Datasheet**: STM32H723 datasheet'inden USART'in alternatif fonksiyon (AF) tablosuna bakın
2. **Pin çakışması**: Seçtiğiniz pin başka bir peripheral'da kullanılmıyor mu?
3. **Nucleo kart jumper'ları**: NUCLEO-H723ZG kartında bazı pinler SB (solder bridge) jumper'ları ile bağlıdır, kontrol edin
4. **Mevcut kullanım**: Mevcut pin kullanımları:
   - PD5/PD6 → MOTOR_FL (USART2)
   - PD8/PD9 → MOTOR_RL (USART3) + ST-LINK VCP (dikkat!)
   - PD0/PD1 → MOTOR_FR (UART4)
   - PC12/PD2 → MOTOR_RR (UART5)
   - PG9/PG14 → Terminal UART (USART8) **VE** FTDI_UART (USART6) — çakışma!
5. **AF numarası**: Her pin'in USART için doğru AF numarasına sahip olduğunu doğrulayın

---

## Değişiklik Özet Tablosu

| # | Dosya | Değişiklik |
|---|-------|------------|
| 1 | `platformio.ini` | `-DENABLE_HWSERIAL7` ekle |
| 2 | `types.h` | `MOTOR_XX = 4` ve `MOTOR_COUNT = 5` ekle |
| 3 | `config.h` | `MOTOR_UART_XX Serial7` makrosu ve `motorName()` güncellemesi |
| 4 | `main.cpp` | Pin atama (setRx/setTx), başlangıç mesajı güncellemesi |
| 5 | `motor_dispatcher.cpp` | `_uartMap[MOTOR_XX] = &MOTOR_UART_XX` ekle |
| 6 | `motion_controller.cpp` | Yön hesaplamasına yeni motoru dahil et |
| 7 | Derle & Yükle | Hata kontrolü yap |

---

## Özet Akış Şeması

```
Yeni Motor Ekleme:
  ┌─────────────────────────────────┐
  │ 1. platformio.ini → build flag  │
  │ 2. types.h     → MotorId enum    │
  │ 3. config.h   → UART makrosu     │
  │ 4. main.cpp   → pin atama        │
  │ 5. motor_dispatcher → uartMap[]   │
  │ 6. motion_controller → yön/pwm    │
  │ 7. Derle & Yükle                  │
  └─────────────────────────────────┘
```

Her adımda ilgili `MOTOR_COUNT` ve dizi boyutlarının güncellendiğinden emin olun. `_uartMap[MOTOR_COUNT]` gibi bildirimler enum'daki `MOTOR_COUNT` değerini kullanır — bu değeri artırmak dizileri otomatik genişletir.