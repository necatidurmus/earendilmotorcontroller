# H7 F446 Import — Test Plan

> Bu dosya `docs/H7_F446_IMPORT_IMPLEMENTATION_PLAN.md` planındaki
> değişikliklerin test prosedürlerini içerir.
> Tüm testler motorsuz veya current-limited bench PSU ile yapılır.

---

## 1. Motorsuz UART Mapping Testi

**Amaç:** H7'nin 4 motor UART'ının doğru maplendiğini doğrula.

**Ön koşul:** F411 bağlı değil. Sadece H7 terminal bağlantısı.

**Test adımları:**
1. `mode manual`
2. `FL status` — TX gönderildi log'u görülmeli (RX boş, link-lost beklenir)
3. `FR status` — aynı
4. `RL status` — aynı
5. `RR status` — aynı
6. `ALL status` (tune olarak) — 4 motora birden gittiği loglanmalı

**Kabul kriteri:**
- `[TX][FL] status`, `[TX][FR] status`, `[TX][RL] status`, `[TX][RR] status`
  log satırları görünmeli
- Hata mesajı olmamalı

---

## 2. Tek F411 ile Port Testi

**Amaç:** Her H7 motor UART portunun F411 ile iletişimini doğrula.

**Ön koşul:** 1 adet F411 motor controller, current-limited PSU (0.3A).

**Test adımları (her FL/FR/RL/RR portu için tekrarla):**
1. F411'i sırayla FL → FR → RL → RR portuna bağla
2. `<port> status` — `[OK]` ile status bilgisi gelmeli
3. `<port> hall` — Hall haritası gelmeli
4. `mode manual`
5. `<port> mode duty` — F411 mode değiştirmeli
6. `<port> f100` — Motor dönmeli (dikkatli, düşük duty)
7. `stop` — Motor durmalı
8. `<port> b100` — Motor ters yönde dönmeli
9. `stop` — Motor durmalı

**Kabul kriteri:**
- Her portta F411'den `[OK]`/status/telemetry yanıtı alınıyor
- Motor komutları doğru portta çalışıyor
- Stop komutu motoru durduruyor

---

## 3. 4 F411 Telemetry Testi

**Amaç:** 4 motorun ayrı telemetry stream'lerini ayırabilmeyi doğrula.

**Ön koşul:** 4 adet F411 bağlı, hepsine `telper 100` ayarlanmış.

**Test adımları:**
1. `mode manual`
2. `ALL telper 100` — 4 motora 100ms telemetry interval
3. `bridge status` — bridge=ON olmalı
4. Terminalda `[TEL][FL]`, `[TEL][FR]`, `[TEL][RL]`, `[TEL][RR]`
   prefixleriyle ayrı satırlar görülmeli
5. `bridge off` — Terminalde telemetry satırları durmalı
6. `bridge on` — Telemetry satırları geri gelmeli
7. Motor TX ve RX activity hala aktif olmalı (link-lost alarm gelmemeli)

**Kabul kriteri:**
- `[TEL][FL] RPM:...`, `[TEL][FR] RPM:...` vs. ayrı ayrı görünüyor
- `bridge off` sonrası terminal log duruyor ama LINK_LOSS alarm yok
- `bridge on` sonrası telemetry geri geliyor

---

## 4. Service Lock Testi

**Amaç:** Service lock mekanizmasının doğru çalıştığını doğrula.

**Test adımları:**
1. `mode manual`
2. `FL identify` — "service locked" mesajı ile bloklanmalı
3. `bridge status` — `service=LOCKED`, `blocked_cmds=1`
4. `bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY` —
   "service unlocked for 30s"
5. `FL identify` — artık çalışmalı (TX log görünmeli)
6. 30 saniye bekle — service otomatik kilitlenmeli
7. `gate test` komutu engellenmeli
8. `service unlock CURRENT_LIMITED_BENCH_SUPPLY` — alias çalışmalı
9. `service lock` — manuel kilitleme
10. `FL base 640 660 680 700 720 700 670 640` — bloklanmalı
11. `service status` — `service=LOCKED`, blocked count artmış olmalı
12. `FL telper 100` — telper güvenli komut, bloklanmamalı

**Kabul kriteri:**
- Service lock kapalıyken: identify, gatetest, base, boost, pi,
  kickduty, kickms, ramp, save, map set/apply/reset/save/load/edit/discard
  hepsi bloklanıyor
- Service unlock sonrası hepsi gidebiliyor
- `telper` güvenli komut, hiç bloklanmıyor
- 30s timeout sonrası otomatik kilitlenme

---

## 5. Stop/Safe/Brake/Estop Testi

**Amaç:** Stop tiplerinin doğru sırayla gönderildiğini ve
overwrite olmadığını doğrula.

**Test adımları:**
1. `mode manual`, `mode duty`
2. `f30` — motor dönmeli
3. `stop` — rpm 0 + stop sırası loglanmalı
4. `f30`
5. `safe` — safe + stop sırası loglanmalı
6. `f30`
7. `brake` — x + stop sırası loglanmalı
8. `f30`
9. `estop` — direkt estop, service lock iptal olmalı
10. **Overwrite testi:**
    - `f30`
    - `stop` (hemen ardından)
    - `safe` (hemen ardından)
    - Her iki komut da sırayla gönderilmeli, ikincisi overwrite olmamalı

**Kabul kriteri:**
- `stop`: `[TX][*] rpm 0` log, drain, `[TX][*] stop` log
- `safe`: `[TX][*] safe` log, drain, `[TX][*] stop` log
- `brake`: `[TX][*] x` log, drain, `[TX][*] stop` log
- `estop`: `[TX][*] estop` log
- Hızlı `stop` + `safe` arka arkaya geldiğinde overwrite yok

---

## 6. DISARM Mode Gate Testi

**Amaç:** DISARM modunda sadece güvenli komutların geçtiğini doğrula.

**Test adımları:**
1. `mode disarm`
2. `FL status` — çalışmalı (safe query)
3. `FL stop` — çalışmalı
4. `FL x` — çalışmalı
5. `FL mode speed` — çalışmalı
6. `FL mode duty` — çalışmalı
7. `stop` — çalışmalı
8. `estop` — çalışmalı
9. `safe` — çalışmalı
10. `bridge status` — çalışmalı
11. `service status` — çalışmalı
12. `service unlock CURRENT_LIMITED_BENCH_SUPPLY` — çalışmalı
13. `FL f100` — bloklanmalı
14. `FL identify` — bloklanmalı (DISARM + service lock)
15. `f100` — bloklanmalı
16. `FL gatetest 0 500` — bloklanmalı
17. `mode manual` — geçiş yapılmalı
18. `FL status` — artık çalışmalı
19. `FL f100` — artık çalışmalı (MANUAL modunda)

**Kabul kriteri:**
- DISARM'da güvenli komutlar geçiyor
- DISARM'da motion/dangerous komutlar bloklanıyor
- Estop, safe, stop, brake DISARM'da izinli
- Bridge ve service komutları DISARM'da izinli
- MANUAL'e geçince motion komutları çalışıyor

---

## 7. RX DMA Line Assembler Testi

**Amaç:** Birden fazla satırın tek DMA chunk'ta geldiğinde
doğru parçalandığını doğrula.

**Test adımları:**
1. F411'de `telper 20` (yüksek rate)
2. `[TEL][FL]` satırları doğru prefix ile geliyor
3. `[OK]` ve `[ERR]` satırları da ayrı satırlar olarak görünüyor
4. Uzun satır (status block) bölünmeden görünüyor

**Kabul kriteri:**
- Her satır ayrı `Logger_Log` çağrısı ile basılıyor
- Satır birleşmesi veya parçalanması yok
- Overflow durumunda uyarı loglanıyor ve sonraki satır düzgün devam ediyor

---

## 8. Backward Compatibility Testi

**Amaç:** Mevcut komutların hala çalıştığını doğrula.

**Test adımları:**
1. `mode manual`, `m speed` — RPM mode
2. `f100` — forward 100 RPM
3. `b100` — backward 100 RPM
4. `l100` — left 100 RPM
5. `r100` — right 100 RPM
6. `stop` — stop
7. `m duty` — PWM mode
8. `fd200` — forward duty 200
9. `bd200` — backward duty 200
10. `stop`
11. `FL status`
12. `FR status`
13. `RL status`
14. `RR status`
15. `mode disarm`
16. `mode manual`

**Kabul kriteri:**
- Tüm komutlar eskisi gibi çalışıyor
- Hata mesajı yok

---

## 9. GUI Test Panel Testi

**Amaç:** GUI'deki 4 motorlu test panelinin düzgün çalıştığını doğrula.

**Test adımları:**
1. GUI'yi başlat (`python earendil.py`)
2. Motor seçimi: `FL` → Status butonuna bas → `FL status` gönderildi
3. `ALL` → Status → `ALL status` gönderilmedi (raw ALL desteklenmiyor,
   sadece tuning) -- alternatif: `FL status`, `FR status` vs.
4. Identify butonuna bas → Onay penceresi açılmalı
5. Onayla → `service unlock CURRENT_LIMITED_BENCH_SUPPLY` + `FL identify`
6. Service Unlock butonu → `service unlock CURRENT_LIMITED_BENCH_SUPPLY`
7. E-stop butonu → `estop`
8. Bridge On/Off butonları

**Kabul kriteri:**
- Her buton doğru komutu gönderiyor
- Identify ve diğer riskli komutlarda onay penceresi çıkıyor
- Motor seçimi FL/FR/RL/RR/ALL çalışıyor
- Terminal log'da komutlar görünüyot

---

## 10. Limit Alignment Testi

**Amaç:** H7 parser limitlerinin F411 ile uyumlu olduğunu doğrula.

**Test adımları:**
1. `FL kickms 500` — kabul edilmeli (≤ 1000)
2. `FL kickms 1500` — reddedilmeli (> 1000, F411 max=1000)
3. `FL boost 800 800 800 800 800 800 800 800 500` — kabul (MS ≤ 1000)
4. `FL boost 800 800 800 800 800 800 800 800 1500` — reddedilmeli
5. `FL telper 50` — kabul (20 ≤ 50 ≤ 5000)
6. `FL telper 10` — reddedilmeli (< 20)
7. `FL telper 6000` — reddedilmeli (> 5000)
8. `f201` — clamped to 200 (rover RPM limit)
9. `FL rpm 500` — raw motorda 500 sınır (F411 limiti)
10. `FL rpm 600` — raw motorda 500'e clamped

**Kabul kriteri:**
- F411 sınırları dışındaki değerler reddediliyor veya clamped
- F411 sınırları içindeki değerler kabul ediliyor
- Rover hareket komutlarında RPM_MAX_ROVER (200), raw motorda RPM_MAX_RAW (500)
