Şimdi önceki kararlarına sadık kalarak projenin tüm dosyalarını eksiksiz yaz.

Kısıtlar:
- Hedef kart: NUCLEO-H723ZG
- PlatformIO
- Arduino framework
- 5 UART yapısı olacak
- 1 UART debug/terminal için
- 4 UART 4 ayrı F411 motor kartı için
- Komutlar:
  - forward <pwm>
  - backward <pwm>
  - left <pwm>
  - right <pwm>
  - stop
  - status
  - help
- PWM aralığı 0-255
- Non-blocking yapı
- String kullanımını minimumda tut
- Char buffer tabanlı satır alma yap
- '\n' ve '\r\n' destekle
- Buffer overflow kontrolü yap
- Geçersiz komutlarda hata mesajı dön
- Her 2 saniyede debug uart üzerinden heartbeat/status özeti gönder
- Açılışta "System started" mesajı gönder
- Debug UART’tan gelen geçerli komutları logla
- Motor UART’larına gönderilen komutları debug UART’a da logla

Dosyalar:
- platformio.ini
- include/board_uart_config.h
- include/uart_manager.h
- src/uart_manager.cpp
- include/command_parser.h
- src/command_parser.cpp
- include/motor_router.h
- src/motor_router.cpp
- src/main.cpp

Kurallar:
1. Her dosyayı tam içerikle yaz
2. Her dosyayı ayrı kod bloğunda ver
3. Placeholder bırakma
4. Sözde kod verme
5. Derlenebilir gerçek örnek ver
6. Önceki seçtiğin UART/pin kararlarına sadık kal
7. En sonda örnek terminal komutları ve beklenen debug/motor uart çıktıları ver