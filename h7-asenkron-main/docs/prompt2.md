Şimdi aynı proje için sadece UART/USART birimleri ve pin seçimini yap. Henüz kod yazma.

Hedef:
- NUCLEO-H723ZG
- PlatformIO
- Arduino framework
- Toplam 5 UART kullanılacak
- 1 UART PC terminal/debug için
- 4 UART 4 ayrı STM32F411 motor kartına komut göndermek için

İstediklerim:
1. DEBUG UART için en uygun USART/UART birimini seç
2. 4 motor UART’ı için en uygun 4 ayrı USART/UART birimini seç
3. Her biri için TX ve RX pinlerini açıkça yaz
4. Eğer ST-LINK Virtual COM Port kullanılabiliyorsa bunu ayrıca belirt
5. Seçtiğin UART/pinlerin neden mantıklı olduğunu kısa teknik gerekçelerle açıkla
6. Arduino framework tarafında bunların Serial, Serial1, Serial2 gibi hangi nesnelere karşılık geldiğini söyle
7. Belirsizlik varsa en güvenli yaklaşımı seç ve bunu açıkça not et
8. Henüz kod yazma
9. En sonda tablo halinde özet ver

Önemli:
- Hatalı veya şüpheli pin eşlemesi yapma
- NUCLEO-H723ZG ve Arduino core ile mümkün olduğunca uyumlu seçim yap