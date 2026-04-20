# BLDC Motor Controller — Hata ve Sorun Raporu

> **Proje:** Asenkron BLDC Motor Controller (STM32 Black Pill F411CE)
> **Dosyalar:** `src/main.cpp`, `tools/wasd_controller.py`
> **Tarih:** 20 Nisan 2026
> **Öncelikler:** 🔴 Kritik  |  🟡 Orta  |  🟢 Düşük

---

## 🔴 KRİTİK HATALAR

### 1. Python Hold-to-Run Mekanizması BOZUK
**Dosya:** `tools/wasd_controller.py:429-433`

```python
elif key == -1:
    if ctrl.key_held is not None:
        ctrl.set_key(None)
        ctrl.stop()
```

**Sorun:** `nodelay(True)` + `timeout(80)` ile `getch()` her 80ms'de bir `-1` döndürür. Tuşa basılı tutarken bile 80ms sonra tuş "bırakıldı" sanılır ve motor durdurulur. Kullanıcı tuşa basılı tutamaz — motor sürekli durur.

**Etki:** WASD kontrolü tamamen çalışmaz. Motor anlık bile dönemez.

**Çözüm:** `key == -1` bloğunu kaldır. Tuş bırakma olayı ayrı bir tuş takibi ile yönetilmeli (önceki tuş durumu ile karşılaştırma).

---

### 2. Komut Kuyruğu Her Döngüde Sadece 1 Komut İşliyor
**Dosya:** `src/main.cpp:1564-1569`, `src/main.cpp:1727-1732`

```cpp
void processQueuedCommands() {
  CommandItem item;
  if (dequeueCommand(&item)) {   // ← if yerine while olmalı
    processCommand(item.text, item.src);
  }
}
```

**Sorun:** `if` ile sadece tek komut işleniyor. Kuyrukta birden fazla komut birikirse, her `loop()` iterasyonunda sadece biri işlenir.

**Etki:** Hızlı komut girişlerinde kuyruk şişer, gecikmeler oluşur.

**Çözüm:** `if` → `while` döngüsüne çevrilerek kuyruk tamamen boşaltılmalı.

---

### 3. Python Modunda Komut Çakışması: "s" = Stop vs Geri
**Dosya:** `src/main.cpp:1724`

```cpp
// processPythonCommand sonu:
processCommand(cmd, src);  // ← bilinmeyen komutlar normal parser'a düşüyor
```

**Sorun:** Python modunda "s" tuşu geri (backward) için kullanılıyor. Ama `processCommand` içinde `strcmp(cmd, "s") == 0` → stop olarak yorumlanıyor.

**Etki:** Python modunda geri gitmek imkansız — motor anında durur.

**Çözüm:** Python modunda `processCommand` çağrısı kaldırılmalı veya "s", "w" gibi tuşlar blacklist'e alınmalı.

---

### 4. applyDriveState Dead-Time Yok (Shoot-Through Riski)
**Dosya:** `src/main.cpp:538-556`

```cpp
if (oldL != newL) digitalWrite(oldL, LOW);
if (oldL != newL) digitalWrite(newL, HIGH);  // ← ölü zaman yok!
analogWrite(newH, dutyCycle);
```

**Sorun:** Düşük taraf MOSFET'i kapatıp diğerini açarken arada ölü zaman bırakılmıyor.

**Etki:** Kısa devre, MOSFET hasarı, aşırı ısınma.

**Çözüm:** `digitalWrite(oldL, LOW)` sonrası 1-5µs bekleme eklenebilir veya donanımsal dead-time sürücü kullanılmalı.

---

## 🟡 ORTA SEVİYE HATALAR

### 5. saveall Komutu Modu Kaydetmiyor
**Dosya:** `src/main.cpp:1416-1428`

**Sorun:** `saveall` sadece hall map ve config'i kaydediyor. `activeMode` EEPROM'a yazılmıyor.

**Etki:** Cihaz yeniden başlatıldığında mod sıfırlanır.

**Çözüm:** `saveModeToStorage()` çağrısı eklenmeli.

---

### 6. Identify Step Geçişi Çok Hızlı
**Dosya:** `src/main.cpp:1249`

```cpp
serviceRt.nextActionMs = now + 1;  // ← 1ms!
```

**Sorun:** Identify bir step'ten diğerine geçerken sadece 1ms bekleme var. Motor fiziksel olarak yanıt veremeden ilerler.

**Etki:** Yanlış hall eşleştirmeleri, geçersiz map üretimi.

**Çözüm:** En az 50-100ms bekleme süresi eklenmeli.

---

### 7. RPM Yorumu Hatalı
**Dosya:** `src/main.cpp:1898`

**Sorun:** Yorum yanlış. Doğrusu: 6 geçiş/elektriksel tur × 15 elektriksel tur/mekanik tur = 90 geçiş/mekanik tur.

---

### 8. Phase İsimleri Eksik (Python)
**Dosya:** `tools/wasd_controller.py:203`

```python
phases = ["STOPPED", "KICK", "RUNNING", "NEUTRAL", "FAULT"]
```

**Sorun:** "NEUTRAL" aslında "NEUTRAL_WAIT" olmalı.

**Etki:** NeutralWait fazında "?" gösterilir.

**Çözüm:** `phases = ["STOPPED", "KICK", "RUNNING", "NEUTRAL_WAIT", "FAULT"]`

---

### 9. pwm_up/pwm_down Local Değeri Güncellemiyor
**Dosya:** `tools/wasd_controller.py:113-119`

**Sorun:** Firmware'a komut gönderiliyor ama `ctrl.pwm_set` local olarak güncellenmiyor.

**Etki:** PWM bar ve değer göstergesi gecikmeli güncellenir.

**Çözüm:** `self.pwm_set = clamp(self.pwm_set + PWM_STEP, 0, 255)` eklenebilir.

---

### 10. Heartbeat Thread Gereksiz / Çalışmıyor
**Dosya:** `tools/wasd_controller.py:127-134`

**Sorun:** `key_held` main loop tarafından anında `None` yapıldığı için heartbeat thread hiçbir zaman komut gönderemez.

**Etki:** Ölü kod. Hata #1 düzeltilirse anlamlı hale gelir.

---

### 11. beginRunRequest Aynı Yönde Duty Güncellemesi Belirsiz
**Dosya:** `src/main.cpp:1003-1010`

**Sorun:** Motor aynı yönde çalışırken yeni duty isteği gelirse `beginRunRequest` erken döner. Akış kafa karıştırıcı.

---

## 🟢 DÜŞÜK ÖNCELİKLİ

### 12. `<cstring>` Include Eksik
**Dosya:** `src/main.cpp:1`

**Sorun:** `memset`, `memmove`, `strlen` vb. kullanılıyor ama `<cstring>` include edilmemiş.

---

### 13. Identify Sonrası Kullanıcıya Bilgi Yok
**Dosya:** `tools/wasd_controller.py:427-428`

**Sorun:** Kullanıcıya "identify başladı" gibi bir geri bildirim verilmiyor.

---

### 14. rxPush/rxPop Atomik Değil
**Dosya:** `src/main.cpp:821-834`

**Sorun:** `head` ve `tail` işaretçileri interrupt-safe değil. Gelecekte ISR'den erişim eklenirse race condition oluşur.

---

## ÖZET TABLOSU

| # | Öncelik | Dosya | Satır | Başlık |
|---|---------|-------|-------|--------|
| 1 | 🔴 | wasd_controller.py | 429-433 | Hold-to-Run bozuk |
| 2 | 🔴 | main.cpp | 1564-1569 | Kuyruk yavaş işliyor |
| 3 | 🔴 | main.cpp | 1724 | Python "s" komut çakışması |
| 4 | 🔴 | main.cpp | 538-556 | Dead-time yok |
| 5 | 🟡 | main.cpp | 1416-1428 | saveall mod kaydetmiyor |
| 6 | 🟡 | main.cpp | 1249 | Identify step çok hızlı |
| 7 | 🟡 | main.cpp | 1898 | RPM yorumu hatalı |
| 8 | 🟡 | wasd_controller.py | 203 | Phase isimleri eksik |
| 9 | 🟡 | wasd_controller.py | 113-119 | PWM local güncelleme yok |
| 10 | 🟡 | wasd_controller.py | 127-134 | Heartbeat ölü kod |
| 11 | 🟡 | main.cpp | 1003-1010 | Duty güncelleme belirsiz |
| 12 | 🟢 | main.cpp | 1 | `<cstring>` eksik |
| 13 | 🟢 | wasd_controller.py | 427-428 | Identify bildirim yok |
| 14 | 🟢 | main.cpp | 821-834 | Ring buffer atomik değil |
