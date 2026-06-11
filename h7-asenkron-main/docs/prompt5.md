Şimdi tüm proje için dosya yapısını ve modüller arası arayüzleri oluştur. Henüz tüm .cpp içeriklerini eksiksiz yazma, önce iskeleti kur.

Hedef:
- NUCLEO-H723ZG
- PlatformIO
- Arduino framework
- 5 UART
- 1 debug uart
- 4 motor uart

İstediğim dosyalar:
- platformio.ini
- include/board_uart_config.h
- include/uart_manager.h
- src/uart_manager.cpp
- include/command_parser.h
- src/command_parser.cpp
- include/motor_router.h
- src/motor_router.cpp
- src/main.cpp

İstediklerim:
1. Önce proje ağacını ver
2. Sonra her dosyanın görevini açıkla
3. Her header için public interface öner
4. Hangi modül hangi modülü kullanacak açıkla
5. main.cpp akışını anlat
6. Henüz tam implementasyon verme
7. Henüz her dosyanın bütün kodunu yazma
8. Önce sadece temiz bir proje planı ve API tasarımı ver