# Earendil BLDC Motor Driver Board — Hardware Overview

## 1. Kartın kısa tanımı

Bu kart, **STM32F411 Black Pill** tabanlı, **hall sensörlü 3 faz BLDC motor sürücü kartıdır**. Güç katında **3 adet half-bridge gate driver** ve **6 adet harici N-kanal MOSFET** ile klasik bir **3 faz inverter / full bridge** yapısı kurulur. Kart üzerinde ayrıca:

- hall sensör girişi,
- akım ölçümü,
- DC bus voltaj ölçümü,
- throttle girişi,
- 5 V regülasyon,
- fuse ve TVS tabanlı temel giriş koruması,
- USB/UART tabanlı bring-up ve test altyapısı

bulunur.

Bu kartın ana kullanım amacı, sensörlü BLDC motoru 6-step komütasyon ile sürmek ve daha sonra yazılım tarafında daha gelişmiş kontrol stratejilerine açık bir donanım temeli sağlamaktır.

---

## 2. Sistem seviyesi genel bakış

Kartı sistem seviyesinde üç ana bölüme ayırmak en doğrusudur:

1. **Kontrol katmanı**
   - STM32F411 Black Pill modülü
   - hall okuma
   - PWM üretimi
   - ADC okuma
   - UART/USB haberleşmesi

2. **Sürücü ve güç katmanı**
   - 3 × L6388 gate driver
   - 6 ×  ırfb7730 N-MOSFET
   - bootstrap ağı
   - DC bus kapasitörleri
   - motor faz çıkışları

3. **Ölçüm ve dış arayüz katmanı**
   - hall konektörü
   - current sense (shunt + INA181)
   - voltage sense
   - throttle girişi
   - ana güç girişi
   - fuse / TVS

Bu ayrım önemli çünkü kullanıcı komutu önce MCU'ya gelir; MCU gate driver girişlerini sürer; gate driver MOSFET'leri sürer; MOSFET'ler üç fazı oluşturur; hall, akım ve voltaj geri beslemeleri yeniden MCU'ya döner.

---

## 3. Ana donanım blokları

## 3.1 MCU modülü

- **Kart:** Black Pill V3
- **MCU ailesi:** STM32F411CE sınıfı
- **Rolü:**
  - hall sensör verisini okumak
  - gate driver girişlerini sürmek
  - ADC üzerinden akım/voltaj almak
  - USB/UART üzerinden komut almak
  - motor kontrol mantığını çalıştırmak

## 3.2 Gate driver bloğu

- **Entegre:** 3 × L6388ED013TR
- **Topoloji:** her faz için bir half-bridge gate driver
- **Rolü:**
  - MCU'dan gelen logic seviyeli sürme sinyallerini MOSFET gate sürüşüne çevirmek
  - high-side ve low-side MOSFET sürüşünü yönetmek
  - bootstrap ile high-side gate sürüşünü mümkün kılmak

## 3.3 Güç katı

- **Topoloji:** 3 faz full bridge
- **Ana elemanlar:** 6 × harici N-channel MOSFET
- **Rolü:** DC bus enerjisini motorun üç fazına dağıtarak dönen manyetik alan üretmek

## 3.4 Ölçüm katı

- **Current sense:** low-side shunt + INA181
- **Voltage sense:** direnç bölücü + ADC
- **Rolü:** koruma, tanı ve ileride closed-loop kontrol altyapısı sağlamak

## 3.5 Dış bağlantı katı

- ana güç girişi
- motor faz çıkışları
- hall konektörü
- throttle konektörü
- USB / UART erişimi

---

## 4. MCU ve pin eşleşmeleri

Aşağıdaki pinler bu kart için en kritik pinlerdir.

| MCU Pin | Fonksiyon | Blok | Açıklama |
|---|---|---|---|
| PA8 | CTRL_AH / High-side A | Gate driver | Faz A üst MOSFET sürüş girişi |
| PA9 | CTRL_BH / High-side B | Gate driver | Faz B üst MOSFET sürüş girişi |
| PA10 | CTRL_CH / High-side C | Gate driver | Faz C üst MOSFET sürüş girişi |
| PA7 | CTRL_AL / Low-side A | Gate driver | Faz A alt MOSFET sürüş girişi |
| PB0 | CTRL_BL / Low-side B | Gate driver | Faz B alt MOSFET sürüş girişi |
| PB1 | CTRL_CL / Low-side C | Gate driver | Faz C alt MOSFET sürüş girişi |
| PB6 | Hall A | Hall input | Hall A dijital girişi |
| PB7 | Hall B | Hall input | Hall B dijital girişi |
| PB8 | Hall C | Hall input | Hall C dijital girişi |
| PA0 | ISENSE | Analog | INA181 çıkışı, akım ölçümü |
| PA4 | VSENSE | Analog | DC bus voltaj ölçümü |
| PA2 | UART TX | Haberleşme | Donanımsal UART hattı |
| PA3 | UART RX | Haberleşme | Donanımsal UART hattı |
| PC13 | LED | Durum | Kart üstü durum LED'i |

### Not

Mevcut proje belgelerinde iki pratik haberleşme yolu görülüyor:

- **Black Pill USB-C / USB CDC tabanlı bring-up kullanımı**
- **PA2 / PA3 üstünden donanımsal UART kullanımı**

Yani kart hem seri tabanlı test, hem de doğrudan MCU pinlerinden UART bağlantısı için uygundur.

---

## 5. Gate driver katı

## 5.1 Kullanılan entegre

- **U8, U9, U10 = L6388ED013TR**
- Her biri bir half-bridge sürer.

Bu nedenle kartta üç adet sürücü vardır:

- faz A half-bridge
- faz B half-bridge
- faz C half-bridge

## 5.2 Neden 3 adet gate driver var?

BLDC motor üç fazlıdır. Her faz için bir **üst MOSFET + alt MOSFET** çifti gerekir. Her faz bacağına bir half-bridge driver koyarak toplamda:

- 3 high-side sürüş,
- 3 low-side sürüş,
- 6 MOSFET kontrolü

elde edilir.

## 5.3 Gate driver girişleri

MCU'nun kontrol pinleri gate driver'ın logic girişlerine bağlanır:

| Gate sinyali | MCU pini | Faz |
|---|---|---|
| CTRL_AH | PA8 | A üst |
| CTRL_AL | PA7 | A alt |
| CTRL_BH | PA9 | B üst |
| CTRL_BL | PB0 | B alt |
| CTRL_CH | PA10 | C üst |
| CTRL_CL | PB1 | C alt |

## 5.4 Gate driver çıkışları

L6388'lerin çıkışları MOSFET gate ağlarına gider:

- DRV_AH / DRV_AL
- DRV_BH / DRV_BL
- DRV_CH / DRV_CL

Bu çıkışlar doğrudan MOSFET drain/source taşımaz; yalnızca gate sürüşünü sağlar.

## 5.5 Bootstrap desteği

Her high-side sürüş için bootstrap diyodu ve bootstrap kapasitörü bulunur.

İlgili elemanlar:

- **D2, D3, D4** — bootstrap diyotları
- **C1, C2, C3 = 1 µF** — bootstrap kapasitörleri

### Neden gerekli?

N-kanal MOSFET'i high-side'da sürebilmek için gate'in source seviyesinin üstüne çıkarılması gerekir. Bootstrap ağı, high-side sürücünün bu gerilimi üretmesini sağlar.

---

## 6. MOSFET güç aşaması

## 6.1 Temel topoloji

Kartta 6 MOSFET ile standart bir **3 faz tam köprü** kurulur:

- Faz A: üst + alt MOSFET
- Faz B: üst + alt MOSFET
- Faz C: üst + alt MOSFET

Motorun üç faz çıkışı da bu köprünün orta noktalarından alınır.

## 6.2 Motor faz terminalleri

Motor faz uçları şu çıkışlara bağlanır:

- **COM_A**
- **COM_B**
- **COM_C**

Bu üç nokta, her half-bridge'in orta noktasıdır.

## 6.3 MOSFET sayısı neden 6?

Çünkü 3 fazlı inverter için her faz bacağında iki MOSFET gerekir:

- biri high-side,
- biri low-side.

3 faz × 2 MOSFET = **6 MOSFET**.

## 6.4 Gate dirençleri

- **R14–R19 = 22 Ω**

Her MOSFET gate yolunda direnç bulunur. Bunlar:

- switching hızını bir miktar sınırlar,
- ringing'i azaltır,
- gate sürücüsünü korumaya yardımcı olur.

## 6.5 MOSFET parça numarası durumu

Bu noktada proje dokümanlarında iki seviye bilgi var:

- bazı erken BOM tabanlı özetlerde MOSFET satırı **generic** bırakılmış,
- daha sonraki proje notlarında kurulu MOSFET'in **IRFB7730** olduğu belirtilmiş.

### Güvenli sonuç

- **Kurulu MOSFET'in IRFB7730 olduğu proje içinde güçlü biçimde kullanılıyor.**
- Yine de final üretim ve kesin donanım dokümantasyonu için **fiziksel kart üzerindeki parça baskısı teyit edilmelidir**.

---

## 7. Güç girişleri ve regülasyon

## 7.1 Ana güç girişi

Ana DC besleme karta iki pinli güç girişinden gelir:

- **U15 = POWER2-1**

Bu giriş, doğrudan güç katını besleyen ana bus hattıdır.

## 7.2 Fuse

- **J3 = Fuse**

Fuse, hatalı bağlantı, aşırı akım veya kısa devre gibi durumlarda ana hattı korumaya yardımcı olur. Ancak tek başına ince ölçekli elektronik koruma yerine geçmez.

## 7.3 TVS koruması

- **D1 = TVS**

TVS elemanı, giriş hattındaki ani gerilim darbelerini bastırmak için kullanılır. Özellikle kablo kaynaklı transient ve bus gürültüsünde faydalıdır.

## 7.4 Bus kapasitörleri

### Bulk elektrolitik kapasitörler
- **C4, C5 = 470 µF**

### Destek / yerel decoupling
- **C21, C22 = 100 µF**
- çeşitli 1 µF destek kapasitörleri

Bu kapasitörler:

- bus ripple'ı azaltır,
- ani akım ihtiyacını destekler,
- switching sırasında besleme kararlılığı sağlar.

## 7.5 5 V regülasyon

- **U11 = L7805**

Bu regülatör ana bus'tan **5 V logic / yardımcı hat** üretir.

Destek kapasitörleri:

- **C21, C22 = 100 µF**
- **C15, C16 = 1 µF**

## 7.6 3.3 V durumu

Black Pill modülü kendi 3.3 V alanını taşır. Dolayısıyla sistemde genel mantık:

- güç katı: ana bus
- yardımcı logic: 5 V
- MCU logic: 3.3 V

şeklindedir.

---

## 8. Hall sensör bağlantısı

## 8.1 Hall konektörü

- **U14 = 5 pin header**

Hall konektörünün tipik hatları şunlardır:

- Hall A
- Hall B
- Hall C
- 5 V
- GND

## 8.2 MCU bağlantısı

Hall sinyalleri MCU'ya şu pinlerden gider:

- PB6 = Hall A
- PB7 = Hall B
- PB8 = Hall C

## 8.3 Conditioning ağı

Hall giriş hattında pull-up ve seri/koruma ağı vardır.

İlgili dirençler BOM'a göre:

- **R1–R6 = 2.2 kΩ**
- **R10, R11 = 47 kΩ**

Bu ağın rolü:

- hall sinyalini kararlı hale getirmek,
- girişleri bias'lamak,
- MCU girişini doğrudan ham kablo hattına bırakmamak.

## 8.4 Hall kodlaması

Firmware tarafında hall kelimesi şu mantıkla kurulmuştur:

- bit0 = Hall A
- bit1 = Hall B
- bit2 = Hall C

Geçerli hall durumları tipik olarak:

- 001
- 101
- 100
- 110
- 010
- 011

Geçersiz durumlar:

- 000
- 111

## 8.5 Hall kablosu yanlış bağlanırsa ne olur?

Yanlış sıra, ters fazlama veya sensör bozukluğu durumunda tipik belirtiler:

- motor kilitlenmesi,
- vuruntulu çalışma,
- yüksek boşta akım,
- zor kalkış,
- ters komütasyon,
- aşırı ses.

---

## 9. Akım ölçüm bloğu

## 9.1 Mimari

Akım ölçümü şu yapı ile yapılır:

- low-side shunt resistor
- INA181 current sense amplifier
- MCU ADC girişi

## 9.2 Ana elemanlar

| Eleman | Değer / Parça | Designator | Görev |
|---|---|---|---|
| Shunt | 0.0005 Ω | R9 | Akıma karşı çok küçük gerilim düşümü üretir |
| Amplifier | INA181 | U2 | Shunt üzerindeki milivolt seviyesini yükseltir |
| ADC pini | PA0 | - | Ölçülen akım sinyalini MCU'ya taşır |

## 9.3 Neden INA181 gerekli?

0.5 mΩ gibi çok düşük bir shunt üzerinde gerilim çok küçüktür:

- 1 A → 0.5 mV
- 2 A → 1.0 mV
- 10 A → 5.0 mV

Bu seviyeler doğrudan ADC için çok küçüktür. INA181 bu farkı kazançla yükseltir.

## 9.4 INA181 varyantı durumu

Belge seviyesinde iki durum var:

- eski özetlerde **gain suffix belirsiz** denmiş,
- daha sonraki proje notlarında **INA181A1 (20 V/V)** kullanıldığı belirtilmiş.

### Donanım dokümantasyonu için güvenli ifade

- Akım yükseltecinin **INA181 sınıfı** olduğu doğrulanmıştır.
- **A1 / A2 / A3 / A4 suffix'i fiziksel parça üstünden teyit edilmelidir.**
- Proje notlarında **INA181A1** kabulü kullanılmaktadır.

## 9.5 Şu anki pratik anlamı

Kart akım ölçümünü donanımsal olarak destekliyor. Firmware tarafında:

- akım ADC okunabiliyor,
- izleme yapılabiliyor,
- ancak final koruma ve gelişmiş current-mode kontrolün yazılımda hangi seviyede aktif olduğu firmware sürümüne bağlı.

---

## 10. Voltaj ölçüm bloğu

## 10.1 Temel yol

DC bus voltajı direnç bölücü üzerinden ölçeklenip MCU'nun ADC pinine verilir.

- **VSENSE → PA4**

## 10.2 İlgili dirençler

Belgeye göre bu blokta şunlar yer alır:

- **R12 = 47 kΩ**
- **R13 = 2.2 kΩ**

Bu oran, yüksek bus gerilimini ADC'nin güvenli seviyesine düşürür.

## 10.3 Neden gerekli?

MCU ADC'si doğrudan yüksek bus gerilimini ölçemez. Önce bölücü ile ölçeklenmesi gerekir. Bu sayede:

- undervoltage tespiti,
- bus izleme,
- telemetri

mümkün olur.

## 10.4 Doğrulama notu

Bölücü teorik olarak tanımlanmış olsa da, kesin dönüşüm oranı:

- şematikten,
- fiziksel karttan,
- multimetre ile bench ölçümünden

son bir kez doğrulanmalıdır.

---

## 11. Throttle girişi

## 11.1 Donanım var mı?

Evet. Kart üzerinde throttle için ayrı bir konektör ve filtre ağı bulunuyor.

- **J1 = 4 pin throttle connector**

## 11.2 İlgili pasifler

- **R7 = 47 kΩ**
- **R8 = 47 kΩ**
- **C9 = 22 nF**

Bu ağ, analog throttle sinyalini filtrelemek ve daha kararlı hale getirmek için düşünülmüş görünüyor.

## 11.3 Şu an aktif kullanılıyor mu?

Donanım mevcut, ancak kartın paylaşılan bring-up firmware'lerinde throttle girişinin aktif kullanımının henüz tam doğrulandığı söylenemez. Pratikte daha çok:

- USB CDC,
- UART,
- host komutları

kullanılmıştır.

Yani throttle bloğu **donanımda hazır**, fakat firmware kullanım seviyesi sürüme göre değişebilir.

---

## 12. UART / USB / debug bağlantıları

## 12.1 USB tarafı

Black Pill üzerindeki USB-C portu, bring-up ve test sırasında en pratik yol olarak kullanılmıştır.

Bu yol üzerinden tipik olarak:

- seri komut gönderme,
- motor ileri/geri/stop,
- PWM ayarı,
- hall ve ADC izleme

yapılmıştır.

## 12.2 Donanımsal UART

Donanımsal UART hatları:

- **PA2 = TX**
- **PA3 = RX**

Bu hatlar:

- harici host bağlantısı,
- USB-UART dönüştürücü,
- embedded üst sistemlerle haberleşme

için uygundur.

## 12.3 Debug kullanım amacı

Bu hatlar şu işlerde kullanılır:

- CLI / komut satırı arayüzü,
- test ve tuning,
- bring-up,
- telemetri,
- host kontrollü sürüş denemeleri.

---

## 13. Motor ve dış bağlantılar

## 13.1 Dış bağlantı özeti

| Konnektör / Hat | Ne bağlanır | Not |
|---|---|---|
| U15 | Ana DC besleme | Güç kaynağı / batarya |
| J3 | Fuse hattı | Ana giriş koruması |
| COM_A / COM_B / COM_C | Motorun 3 fazı | BLDC stator uçları |
| U14 | Hall sensör kablosu | Hall A/B/C + 5V + GND |
| J1 | Throttle | Analog throttle / el gazı |
| USB-C | PC / host | Bring-up ve seri izleme |
| PA2 / PA3 | UART | Harici seri kontrol |

## 13.2 Motor fiziksel olarak nereye bağlanır?

BLDC motorun üç faz ucu şu terminallere bağlanır:

- A fazı → COM_A
- B fazı → COM_B
- C fazı → COM_C

Hall sensör kablosu da ayrı hall konektörüne bağlanır.

## 13.3 Hall kablosunda beklenen sinyaller

Tipik olarak:

- 5 V
- GND
- Hall A
- Hall B
- Hall C

Kablo sırası motor üreticisine göre değişebilir; bu yüzden fiziksel pin sırası mutlaka teyit edilmelidir.

---

## 14. Donanım sinyal akışı

Aşağıdaki akış, bir komutun donanımda nasıl motor sürüşüne dönüştüğünü anlatır.

### 14.1 Komuttan MOSFET'e giden yol

1. Kullanıcı veya host bir komut gönderir.
2. Komut MCU'ya USB veya UART üzerinden gelir.
3. MCU kontrol yazılımı hangi fazların sürüleceğine karar verir.
4. MCU, ilgili high-side / low-side kontrol pinlerini sürer.
5. Bu logic sinyaller L6388 gate driver girişlerine gider.
6. L6388'ler MOSFET gate'lerini sürer.
7. 6 MOSFET'li inverter, motorun üç fazında uygun gerilim kombinasyonunu oluşturur.
8. Rotor konuma göre hall sensörleri yeni durum bildirir.
9. Hall, akım ve voltaj bilgisi yeniden MCU'ya döner.

### 14.2 Ölçüm geri besleme yolu

- motor akımı → shunt → INA181 → PA0 ADC
- bus voltajı → divider → PA4 ADC
- rotor konumu → hall sensör → PB6/PB7/PB8

---

## 15. Güç akışı

Aşağıdaki akış, kartın enerji tarafını özetler.

1. Ana DC besleme güç girişinden karta girer.
2. Giriş hattı fuse ve TVS tarafından temel seviyede korunur.
3. Ana bus, MOSFET güç katına ve gate driver besleme ağına dağılır.
4. Aynı bus'tan L7805 ile 5 V yardımcı hat üretilir.
5. Black Pill modülü kendi 3.3 V mantık alanını oluşturur.
6. Gate driver ve MOSFET katı, bu enerji ile motor fazlarını sürer.
7. Motorun çektiği akım shunt üzerinden ölçülür.

### Güç akışı özeti

| Aşama | Açıklama |
|---|---|
| Giriş | DC bus karta girer |
| Koruma | Fuse + TVS |
| Dağıtım | Bus, güç katına ve yardımcı beslemelere dağılır |
| Regülasyon | 5 V logic / yardımcı hat oluşturulur |
| Sürüş | Gate driver + MOSFET köprüsü motoru sürer |
| Geri besleme | Shunt ve voltaj bölücü ile ölçüm alınır |

---

## 16. İlk enerji verme ve bench bağlantı rehberi

Bu kartı ilk kez masada çalıştırırken aşağıdaki sıra izlenmelidir.

## 16.1 Güç vermeden önce

- PCB'yi gözle kontrol et
- MOSFET yönleri doğru mu bak
- gate-source kısa devresi var mı kontrol et
- ana bus kısa devresi var mı ölç
- shunt doğru lehimli mi kontrol et
- bootstrap diyot ve kapasitörler yerinde mi doğrula
- hall konektörü pin sırasını teyit et

## 16.2 İlk güç verme

- düşük voltaj kullan: örneğin 12 V
- akım limitli güç kaynağı kullan
- ilk açılışta düşük akım limiti seç
- USB/seri bağlantı ile MCU'nun çalıştığını doğrula
- LED ve seri log gözle

## 16.3 Hall doğrulama

- motoru elle döndür
- hall durumlarını seri taraftan izle
- 000 / 111 gibi geçersiz kodların kalıcı gelmediğini doğrula

## 16.4 İlk motor denemesi

- düşük PWM ile başla
- kısa süreli sürüş dene
- akımı izle
- anormal vuruntu, faz ısınması veya aşırı ses varsa hemen durdur

## 16.5 Throttle yerine seri komutla başla

İlk bring-up için throttle yerine seri komutla başlamak daha güvenlidir. Çünkü:

- komutlar daha izlenebilir,
- duty daha kontrollü artırılır,
- hata ayıklama kolaylaşır.

---

## 17. Donanımsal riskler ve dikkat noktaları

## 17.1 En riskli bring-up hataları

- hall faz sırası yanlışlığı
- motor faz sırası yanlışlığı
- hatalı high-side / low-side sürüşü
- yanlış bootstrap bağlantısı
- gate sürüşünün çakışması
- yetersiz dead-time veya hatalı firmware
- yüksek duty ile erken test

## 17.2 Ters bağlantı koruması

Kartta fuse ve TVS bulunması faydalıdır; ancak bunlar **tam bir ters polarite koruma çözümü** anlamına gelmez. Giriş korumasının kesin seviyesi fiziksel şemadan ayrıca doğrulanmalıdır.

## 17.3 Overcurrent durumu

Donanımda current sense var, ancak sistemin gerçek güvenlik seviyesi yalnızca donanıma değil firmware'in:

- akım limiti,
- hard fault,
- stop davranışı,
- brake davranışı

uygulamasına da bağlıdır.

## 17.4 L6388'in avantajı

L6388 kullanımı şu açılardan önemlidir:

- logic seviyeli MCU çıkışlarını gate sürüşüne dönüştürür
- high-side sürüşünü bootstrap ile mümkün kılar
- güç katını doğrudan MCU pinine bağlamamış olursun
- doğru tasarlanırsa switching güvenliğini artırır

## 17.5 Fren / stop / fault davranışı

Donanım, aktif sürüş ve olası aktif fren stratejilerine izin verebilir; ancak hangi davranışın güvenli olduğu firmware kararına bağlıdır.

Özellikle:

- coast stop,
- dynamic brake,
- fault all-off,
- watchdog timeout

senaryoları donanım + yazılım birlikte değerlendirilmelidir.

---

## 18. Doğrulanmış / güçlü olasılık / belirsiz öğeler

## 18.1 Doğrulanmış öğeler

| Öğe | Durum | Not |
|---|---|---|
| Black Pill V3 STM32F411 tabanlı yapı | DOĞRULANDI | Proje özetleri ve pin mapping ile tutarlı |
| 3 × L6388 gate driver | DOĞRULANDI | U8/U9/U10 |
| 6 MOSFET'li 3 faz inverter yapısı | DOĞRULANDI | Güç topolojisi net |
| Hall girişleri PB6/PB7/PB8 | DOĞRULANDI | Çalışan pin mapping mevcut |
| ISENSE = PA0 | DOĞRULANDI | Current sense yolu tanımlı |
| VSENSE = PA4 | DOĞRULANDI | Voltaj ölçümü yolu tanımlı |
| 0.5 mΩ shunt | DOĞRULANDI | R9 |
| L7805 tabanlı 5 V hattı | DOĞRULANDI | U11 |
| Hall konektörü, throttle konektörü, güç girişi | DOĞRULANDI | Kart bloklarında mevcut |

## 18.2 Güçlü olasılık / proje içinde kabul edilen bilgiler

| Öğe | Durum | Not |
|---|---|---|
| Kurulu MOSFET = IRFB7730 | GÜÇLÜ OLASILIK | Proje notlarında doğrulandı kabulü var; fiziksel baskı yine kontrol edilmeli |
| INA181 varyantı = INA181A1 | GÜÇLÜ OLASILIK | Daha yeni belgelerde A1 kullanımı var |
| Kartın hem asenkron 6-step hem de daha gelişmiş timer tabanlı sürüşe uygun olması | GÜÇLÜ OLASILIK | Gate routing ve pin dağılımı bunu destekliyor |

## 18.3 Belirsiz / doğrulanmalı noktalar

| Öğe | Durum | Not |
|---|---|---|
| MOSFET'in fiziksel tam parça kodu | BELİRSİZ / DOĞRULANMALI | Kart üstü marking ile teyit edilmeli |
| INA181 suffix | BELİRSİZ / DOĞRULANMALI | A1/A2/A3/A4 kesinleşmeli |
| VSENSE bölücü oranının son doğrulaması | BELİRSİZ / DOĞRULANMALI | Multimetre ile bench ölçümü gerekli |
| Throttle ADC final firmware eşleşmesi | BELİRSİZ / DOĞRULANMALI | Donanım var, yazılım kullanımı sürüme bağlı |
| Nihai giriş voltajı ve termal sınırlar | BELİRSİZ / DOĞRULANMALI | Bench ve termal test gerekir |

---

## 19. Bu kartı bir ekip arkadaşına 2 dakikada nasıl anlatırsın?

Bu kart, Black Pill STM32F411 kullanan, sensörlü 3 faz BLDC motor sürücü kartı. Üç tane L6388 gate driver ile altı harici MOSFET sürüyor ve üç fazlı inverter oluşturuyor. Motorun üç fazı COM_A/COM_B/COM_C'ye bağlanıyor, rotor konumu hall sensörlerden PB6/PB7/PB8 üzerinden okunuyor. Akım, low-side shunt ve INA181 ile PA0'dan; voltaj da bölücü üzerinden PA4'ten ölçülüyor. Kartta throttle donanımı var ama bring-up aşamasında çoğunlukla USB veya UART ile komut verilmiş. Yani bu kart hem temel sensörlü 6-step sürüş hem de daha gelişmiş firmware denemeleri için güçlü bir donanım tabanı sağlıyor.

---

## 20. Bu kartı ilk kez masada çalıştıracak biri hangi sırayla ilerlemeli?

1. Kartı görsel olarak incele.
2. Ana güç girişinde kısa devre olmadığını ölç.
3. MOSFET drain-source ve gate-source yönlerini kontrol et.
4. Bootstrap ve gate driver çevresini doğrula.
5. Hall konektörü pin sırasını teyit et.
6. USB ile MCU'nun ayağa kalktığını test et.
7. Düşük voltaj ve akım limitli PSU ile ilk enerjilemeyi yap.
8. Hall verisini elle çevirerek doğrula.
9. Önce çok düşük duty ile motoru kısa süreli sür.
10. Akımı ve ısınmayı izle.
11. Hatalı ses, yüksek akım veya sert vuruntu görürsen hemen stop et.
12. Hall ve faz sırası doğrulanmadan yüksek duty denemesine geçme.

---

## 21. Sonuç

Bu kart, donanım olarak iyi ayrıştırılmış bir sensörlü BLDC sürücü platformudur. Ana güçlü yanları:

- Black Pill STM32F411 tabanlı kontrol,
- 3 × L6388 ile doğru gate driver ayrımı,
- 6 MOSFET'li gerçek 3 faz güç katı,
- hall, akım ve voltaj geri bildirimi,
- throttle ve seri bring-up seçenekleri,
- geliştirmeye açık bir donanım altyapısı.

En önemli açık doğrulama noktaları ise şunlardır:

- fiziksel kurulu MOSFET ve INA181 varyantı,
- voltaj bölücü oranı,
- throttle hattının firmware'deki son kullanımı,
- gerçek akım/termal sınırlar,
- üretim öncesi güvenlik ve bench doğrulamaları.

Bu nedenle kart, hem bring-up hem de ileri firmware geliştirme için güçlü bir temel sunar; ancak final donanım dokümantasyonu için birkaç kritik noktanın fiziksel kart üstünden son kez teyit edilmesi gerekir.
