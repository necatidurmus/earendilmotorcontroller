# Bug Raporu — BLDC Motor Controller

> Tarih: 2026-06-22  
> Kapsam: f411-motor-cube, h7-main, tools/, dokümantasyon  
> Durum: Düzeltme yapılmadı — sadece tespit

---

## İçindekiler

1. [Kritik Hatalar](#1-kritik-hatalar)
2. [Yüksek Öncelikli Hatalar](#2-yüksek-öncelikli-hatalar)
3. [Orta Öncelikli Hatalar](#3-orta-öncelikli-hatalar)
4. [Düşük Öncelikli Hatalar](#4-düşük-öncelikli-hatalar)
5. [Dokümantasyon / Kod Uyumsuzlukları](#5-dokümantasyon--kod-uyumsuzlukları)

---

## 1. Kritik Hatalar

### K1 — IWDG Watchdog Timeout ~32sn (2sn Olması Gerekiyor)

| | |
|---|---|
| **Dosya** | `h7-main/src/main.cpp:122` |
| **Etki** | Güvenlik — Motor donanırsa 32sn reset bekleme |
| **Neden** | `IWatchdog.begin(IWDG_TIMEOUT_MS * 1000)` — library zaten ms cinsinden alıyor, `*1000` ile 2.000.000ms (~33dk) gönderiliyor, hardware 32.7sn'ye kırpıyor |

```cpp
// Mevcut (hatalı)
IWatchdog.begin(IWDG_TIMEOUT_MS * 1000);

// Olması gereken
IWatchdog.begin(IWDG_TIMEOUT_MS);
```

---

### K2 — terminal.py RPM Modu Hareketi Tamamen Bozuk

| | |
|---|---|
| **Dosya** | `tools/terminal.py:500` |
| **Etki** | RPM/hız modunda klavye/dugme ile hareket çalışmıyor |
| **Neden** | `rpm forward 23` formatında komut üretiliyor — firmware bunu tanımıyor. Doğrusu `rpm 23` (pozitif=ileri, negatif=geri) |

```python
# Mevcut (hatalı)
return f"rpm {motion_name} {self._get_rpm()}"

# Olması gereken — motion_name'i signed integer'a çevir
rpm = self._get_rpm()
if motion_name == "backward":
    rpm = -rpm
return f"rpm {rpm}"
```

---

### K3 — Duty Mode Yön Değiştirme Sessizce Yoksayılıyor

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/app_main.c:385-488` |
| **Etki** | Motor çalışırken `f`/`b` ile yön değiştirme çalışmıyor |
| **Neden** | Komut işleyici `s_app.direction`'ı önce değiştiriyor, sonra `service_motor()` eski yönle aynı olduğunu görüyor ve yön değişikliğini algılamıyor. `rpm` komutu doğru çalışıyor çünkü eski hedefi önce okuyor. |

**Akış (hatalı):**
1. Motor FWD yönünde çalışıyor
2. `b` komutu gelir → `s_app.direction = DIR_REV` (satır 429)
3. `service_motor()` çalışır → `req_dir` zaten REV, `s_app.direction != req_dir` **false**
4. Yön değişikliği algılanamaz → motor eski yönde devam eder

---

## 2. Yüksek Öncelikli Hatalar

### Y1 — Forward Komutunda `pending_direction` Set Edilmiyor

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/app_main.c:406 vs 428` |
| **Etki** | Neutral fazında forward komutu yanlış yöne gidebilir |
| **Neden** | Backward komutu `pending_direction = -1` set ediyor (satır 428) ama forward komutu `pending_direction = +1` set etmiyor (satır 406). Bir önceki reverse komutundan kalan `pending_direction` kullanılır. |

---

### Y2 — Aynı Sector'da Gate Disable/Enable (PWM Kaybı)

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/motor_driver.c:303-305` |
| **Etki** | Her kontrol döngüsünde ~%5 PWM kaybı, tork ripple, sesli gürültü |
| **Neden** | `ApplyStep` her çağrıldığında aynı sector ise bile `phase_high_pwm()` hem CCxE hem CCxNE'yi devre dışı bırakıp yeniden açıyor. Doğrusu sadece CCR güncellemek. |

```c
// Mevcut (hatalı — her döngüde gate glitch)
phase_high_pwm(new_high, s_ccr_ticks);

// Olması gereken — doğrudan CCR yaz
*s_phase[new_high].ccr = s_ccr_ticks;
```

---

### Y3 — terminal.py `rpm stop` Komutu Tanınmıyor

| | |
|---|---|
| **Dosya** | `tools/terminal.py:532, 542, 548, 555, 608` |
| **Etki** | Hız modunda durdurma çalışmıyor |
| **Neden** | `rpm stop` gönderiliyor ama firmware `rpm 0` bekliyor. Birden fazla satırda tekrar ediyor. |

---

### Y4 — Debug Telemetride `Tcmd` Key Case Uyumsuzluğu

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_client.py:63` |
| **Etki** | Debug modunda hedef RPM hep 0 görünür |
| **Neden** | Firmware `Tcmd:` (karışık case) gönderiyor ama kod `TCMD` ile arıyor. `parse_telemetry` key'leri olduğu gibi saklıyor, büyük harfe çevirmiyor. |

```python
# Mevcut (hatalı)
target_str = telem.get("T", telem.get("TCMD", ""))

# Olması gereken
target_str = telem.get("T", telem.get("Tcmd", ""))
```

---

### Y5 — Klavye Kısayolları Entry Widget'ında Yazarken Tetikleniyor

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_gui.py:876-884` |
| **Etki** | RPM girişine "fsb" yazınca ileri/dur/geri tetiklenir — tehlikeli |
| **Neden** | `terminal.py` focus kontrolü yapıyor ama `ftdi_h7_gui.py` yapmıyor. |

---

### Y6 — Motor Yanıt Okuma/Polling Stub

| | |
|---|---|
| **Dosya** | `h7-main/src/motor_dispatcher.cpp:156-169` |
| **Etki** | H7, F411'den gerçek telemetri/status alamıyor |
| **Neden** | `readResponse()` ve `pollAllResponses()` hep false/0 döndürüyor — stub implementasyon. |

---

### Y7 — `f`/`b` Komutu `pwm` Değerini Kullanmıyor

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_gui.py:690-706` |
| **Etki** | Kullanıcı PWM ayarlayıp ileri basarsa beklenen hız oluşmaz |
| **Neden** | `pwm 200` gönderip sonra `f` gönderiliyor. Ama `f` `defpwm` kullanıyor, `pwm` komutu ayrı bir değişkeni etkiliyor. Doğrusu `f200` göndermek. |

---

## 3. Orta Öncelikli Hatalar

### O1 — `fault_manager.c` Volatile Eksik

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/fault_manager.c:12-13` |
| **Neden** | `s_last` ve `s_last_time_ms` ISR'dan yazılıyor ama `volatile` yok. Compiler optimizasyonu main loop'da eski değeri okuyabilir. |

---

### O2 — `GetFilteredRpm()` Çoklu Çağrıda Filtre Bozulması

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/hall_sensor.c:334-341` |
| **Neden** | `GetFilteredRpm()` her çağrılışta low-pass filtre uyguluyor (side-effect). Bir loop döngüsünde SpeedPI + Telemetry tarafından iki kez çağrılırsa filtre iki kez uygulanır, alpha değeri değişir. |

---

### O3 — `mapreset`/`reload` Motor Çalışırken Harita Değiştiriyor

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/app_main.c:743, 751` |
| **Neden** | Bu komutlar motor çalışırken Hall-to-sector haritasını değiştiriyor. Bir sonraki Hall edge'de yanlış komütasyon oluşabilir. `identify`/`scan`/`test` motor durdurulmuş olmayı kontrol ediyor ama bunlar etmiyor. |

---

### O4 — Duplicate Sector Tabloları

| | |
|---|---|
| **Dosya** | `bldc_commutation.c:70-71` ve `motor_driver.c:214-215` |
| **Neden** | Her iki dosya bağımsız forward drive tablosu tutuyor. Senkronize kalmazsa komütasyon ile gate drive uyuşmaz. Compile-time assertion yok. |

---

### O5 — terminal.py TOCTOU Race

| | |
|---|---|
| **Dosya** | `tools/terminal.py:63-68, 74` |
| **Neden** | `is_connected()` lock dışında kontrol ediliyor, ardından `send_line()` lock içinde `self.ser.write()` çağrılıyor. Arada `disconnect()` çalışırsa `AttributeError` fırlatır. |

---

### O6 — ftdi_h7_client.py `int()` Dönüşümünde Hata Yönetimi Yok

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_client.py:61-75` |
| **Neden** | `telem_to_display()` içindeki `int()` çağrıları `ValueError` fırlatabilir. Reader thread'i çöker ve tüm telemetri kesilir. |

---

### O7 — `all_lines_queue.put()` Bloklayıcı

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_client.py:245` |
| **Neden** | `maxsize=2000` kuyruğu doluysa `put()` bloklanır. Okuyucu thread durur, tüm telemetri işleme askıya alınır. |

---

### O8 — terminal.py Serial Buffer Sınırsız Büyüyor

| | |
|---|---|
| **Dosya** | `tools/terminal.py:80` |
| **Neden** | `_reader_loop` içinde `buffer += chunk` ile byte string sınırsız büyüyor. Newline gelmeyen veri akışında bellek taşması riski. |

---

### O9 — ftdi_h7_gui.py Tüm Exception'ları Yutuyor

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_gui.py:892-898, 824-825` |
| **Neden** | `_tick` ve `_service_task` genel `except Exception: pass` kullanıyor. Program hataları sessizce gizleniyor. |

---

### O10 — H7 Bare `stop` Komutu `rpm 0` Göndermiyor

| | |
|---|---|
| **Dosya** | `h7-main/src/main.cpp:418-425` |
| **Neden** | `stop` komutu `handleMotionCommand`'a gider, sadece `"stop"` yazar. `stopAll()` ise önce `"rpm 0"` sonra `"stop"` yazar. Tutarlılık eksikliği. |

---

### O11 — Static Local Buffer Pointer

| | |
|---|---|
| **Dosya** | `h7-main/src/terminal_interface.cpp:44-49` |
| **Neden** | `readLine()` static local buffer pointer'ı döndürüyor. Bir sonraki çağrıda önceki pointer geçersizleşiyor. Reentrant değil. |

---

### O12 — H7 Duty Aralığı 0-400 Ama F411 250'de Kesiyor

| | |
|---|---|
| **Dosya** | `h7-main/config.h:75` vs `f411-motor-cube/app_main.c:440` |
| **Neden** | H7 `DRIVE_VALUE_MAX=400` kabul ediyor, F411 `duty > 250` ise 250'ye kırpıyor. 251-400 arası sessizce kırpılır. |

---

### O13 — Dead Code: `s_isr_tim1_brk`

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/app_main.c:119` |
| **Neden** | `volatile bool s_isr_tim1_brk` ISR'da set ediliyor ama hiçbir yerde okunmuyor. Dead variable. |

---

### O14 — Dead Code: `s_line_usb`

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/uart_protocol.c:65` |
| **Neden** | `static LineBuilder s_line_usb` hiç kullanılmıyor. 64 byte boşa harcanıyor. |

---

### O15 — Binary Protocol Dead Code

| | |
|---|---|
| **Dosya** | `h7-main/src/protocol_handler.h/cpp` |
| **Neden** | `ProtocolHandler` sınıfı hiçbir dosya tarafından `#include` edilmiyor. Tüm binary paket kodu çalışıyor değil. |

---

## 4. Düşük Öncelikli Hatalar

### D1 — `brake_hold_ms` Flash'tan 0 Yüklenirse Anında Timeout

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/app_main.c:1101` |
| **Neden** | Flash'tan `brakeHoldMs = 0` yüklenirse brake timeout ilk döngüde tetiklenir. Aralık kontrolü yok. |

---

### D2 — Neutral Timeout Tick Wrap'ta Takılabilir

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/app_main.c:1091` |
| **Neden** | `HAL_GetTick() >= s_app.neutral_release_ms` mutlak karşılaştırma kullanıyor. ~49.7 günde tick wrap'ta karşılaştırma yanlış olur. Subtraction tabanlı olmalı. |

---

### D3 — `HAL_TIM_GenerateEvent` Return Kontrolü Yok

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/motor_driver.c:237` |
| **Neden** | `HAL_TIM_GenerateEvent` başarısız olursa shadow register'lar güncellenmez. İlk PWM döngüsü eski CCR değerleriyle çalışır. |

---

### D4 — `abs(INT32_MIN)` Tanımsız Davranış

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/telemetry.c:80`, `speed_pi.c:189` |
| **Neden** | `SpeedPI_GetRawTargetRpm()` INT32_MIN döndürürse `abs(INT32_MIN)` C standardına göre tanımsız. Pratikte düşük risk çünkü değerler kilitliyor. |

---

### D5 — `invalidTransitionCount` Periyodik Sıfırlama Belgelenmemiş

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/hall_sensor.c:271-273` |
| **Neden** | Her 100 geçerlide geçersiz sayaç sıfırlanıyor. 100'de 1 gürültü yapan motor asla eşiğe ulaşamaz. Davranış belgelenmemiş. |

---

### D6 — `INVALID_HALL_HOLD_US` Dead Define

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Inc/app_config.h:108` |
| **Neden** | Tanımlı ama hiçbir kaynak dosyada referansı yok. Kullanılmayan sabit. |

---

### D7 — terminal.py Console Widget Sınırsız Büyüyor

| | |
|---|---|
| **Dosya** | `tools/terminal.py:798` |
| **Neden** | Uzun oturumlarda tüm log satırları birikiyor. Bellek kullanımı artar, render yavaşlar. |

---

### D8 — `cmd_simple` Hata Olsa Bile `[OK]` Yazdırıyor

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_emulator.py:57` |
| **Neden** | Firmware `[ERR]` dönse bile fonksiyon `[OK] Tamamlandı` yazdırıyor. Yanlış onay. |

---

### D9 — Import Tight Loop İçinde

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_emulator.py:206` |
| **Neden** | `from ftdi_h7_client import parse_telemetry` her telemetri satırında çağrılıyor. Module-level olmalı. |

---

### D10 — SIGINT Handler Restore Edilmiyor

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_emulator.py:358` |
| **Neden** | `signal.signal(SIGINT, sigint_handler)` ile atanan handler, `main()` dönüşünde restore edilmiyor. |

---

### D11 — Sadece Linux Portalgı Buluyor

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_gui.py:36` |
| **Neden** | `/dev/ttyUSB*` ve `/dev/ttyACM*` ile Linux'a özgü. Windows (`COM*`) veya macOS (`/dev/cu.*`) desteksiz. |

---

### D12 — `motorId % MOTOR_COUNT` Wrap

| | |
|---|---|
| **Dosya** | `h7-main/src/status_manager.cpp:42` |
| **Neden** | `getMotorId >= MOTOR_COUNT` ise modulo ile sessizce wrap olur. Yanlış motor durumu döner. Bounds check olmalı. |

---

### D13 — `errorCount` uint8_t Taşma

| | |
|---|---|
| **Dosya** | `h7-main/src/status_manager.cpp:60-68` |
| **Neden** | Yanıt alınamayan motor için `errorCount` artar. 255'ten sonra 0'a wrap olur. |

---

### D14 — `ramp` Komutu Minimum Değer Clamp'i Belgelenmemiş

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/speed_pi.c:296-297` |
| **Neden** | `ramp 0 0` gönderildiğinde her iki değer de 1.0'a clamp ediliyor ama PROTOCOL.md bunu belirtmiyor. |

---

### D15 — `gatetest` Trailing Garbage Yutuluyor

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/app_main.c:809-812` |
| **Neden** | `gatetest 3 50 extra_garbage` komutu sessizce çalışır. Endptr kontrolü yok. |

---

### D16 — TIM1 UP ve BRK ISR Aynı Öncelik

| | |
|---|---|
| **Dosya** | `f411-motor-cube/Core/Src/tim.c:98-100` |
| **Neden** | Her ikisi de öncelik 5,0. Nested olamaz. BRK ISR çalışırken TIM1 Update bekler. |

---

### D17 — EXTI Hall (Prio 6) USART2/DMA'dan (Prio 5) Düşük

| | |
|---|---|
| **Dosya** | `gpio.c:39` vs `usart.c:81` |
| **Neden** | Hall edge, UART ISR çalışırken beklemek zorunda kalabilir. Mevcut hızda önemli değil ama risk oluşturuyor. |

---

### D18 — `E-Stop` Sırasında `stop` Komutu Engelleniyor

| | |
|---|---|
| **Dosya** | `h7-main/src/main.cpp:246-276` |
| **Neden** | E-Stop aktifken `CMD_STOP` ve `CMD_STOP_ALL` sessizce yutuluyor. İdempotent komutlar engellenmemeli. |

---

### D19 — `safe_stop()` GUI Ana Thread'ini Bloklıyor

| | |
|---|---|
| **Dosya** | `tools/ftdi_h7_client.py:181-185` |
| **Neden** | `time.sleep(0.02)` ile 3 kez bloklama, toplam ~60ms. Pencere kapanışında donma. |

---

### D20 — E-Stop Sırası Telemetri-Komut Kesişmesi

| | |
|---|---|
| **Dosya** | `h7-main/src/main.cpp:202-203` |
| **Neden** | Wheel bridge telemetrisi komut yanıtlarıyla iç içe geçiyor. terminal.py protokolünü karıştırabilir. |

---

## 5. Dokümantasyon / Kod Uyumsuzlukları

### U1 — README.md PWM Frekansı Eski (15kHz → 20kHz Güncellenmemiş)

| | |
|---|---|
| **Dosya** | `README.md:46` |
| **Gerçek** | ISSUE-041 ile ARR 6399→4799改变di, PWM 20kHz oldu |
| **Sorun** | README hâlâ "15 kHz" yazıyor |

---

### U2 — README.md PI Default Değerleri Eski

| | |
|---|---|
| **Dosya** | `README.md:51` |
| **Gerçek** | Kp=0.8, Ki=0.05, max PWM=180 (ISSUE-040 sonrası) |
| **Sorun** | README hâlâ Kp=0.6, Ki=0.0, max=100 yazıyor |

---

### U3 — ROADMAP.md Phase 2/5 Değerleri Eski

| | |
|---|---|
| **Dosya** | `ROADMAP.md:55, 108` |
| **Sorun** | ARR=6399 ve eski PI default değerleri referansları güncel değil |

---

### U4 — PROTOCOL.md `loadcfg` ve `defaults` Komutları Eksik

| | |
|---|---|
| **Dosya** | `docs/PROTOCOL.md` |
| **Gerçek** | Kodda bu komutlar mevcut (app_main.c:834-858) ama PROTOCOL.md'de tanımlı değil |

---

### U5 — Duty Aralığı Tutarsızlığı (0..255 vs 0..250)

| | |
|---|---|
| **Dosya** | `PROTOCOL.md`, `AGENTS.md`, `motor_driver.c:280` |
| **Gerçek** | Kod duty'yi 250'ye kırpıyor |
| **Sorun** | Dokümanlar hâlâ 0..255 yazıyor |

---

### U6 — `test` Komutu Motor Disconnected Uyarısı Eksik

| | |
|---|---|
| **Dosya** | `docs/PROTOCOL.md:79`, `app_main.c:784` |
| **Gerçek** | `gatetest` motor uyarısı yapıyor ama `test` yapmıyor |
| **Sorun** | Her ikisi de motor disconnected için tasarlandı ama sadece biri uyarıyor |

---

### U7 — H7 vs F411 Değer Tutarsızlıkları

| Parametre | H7 | F411 | |
|---|---|---|---|
| `RPM_MAX` | 400 | 500 | F411 daha yüksek — H7 asla 400+ göndermez |
| `PWM_MAX` | 255 | 250 | H7 251-255 gönderebilir, F411 sessizce kırpar |

---

### U8 — motor_driver.c Yorum Eski

| | |
|---|---|
| **Dosya** | `f411-motor-cube/App/Src/motor_driver.c:280` |
| **Sorun** | Yorum "Map 0..255" diyor ama aslında 0..250 |

---

## İstatistikler

| Kategori | Sayı |
|----------|------|
| Kritik | 3 |
| Yüksek | 7 |
| Orta | 15 |
| Düşük | 20 |
| Dokümantasyon | 8 |
| **Toplam** | **53** |

---

## Öncelik Sıralaması (Düzeltme Önerisi)

1. **K1** — IWDG timeout düzelt (güvenlik)
2. **K2** — terminal.py RPM komut formatı düzelt
3. **K3** — Duty mode yön değiştirme düzelt
4. **Y2** — motor_driver.c gate glitch düzelt
5. **Y5** — ftdi_h7_gui.py focus guard ekle
6. **Y4** — ftdi_h7_client.py Tcmd case düzelt
7. **K1 → Y3** — terminal.py `rpm stop` → `rpm 0` düzelt
8. **O1** — fault_manager.c volatile ekle
9. **O3** — mapreset/reload motor çalışırken engelle
10. **U1-U8** — Dokümantasyonu güncelle
