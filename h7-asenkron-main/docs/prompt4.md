Şimdi sadece motor yönlendirme katmanını tasarla. Henüz tüm proje kodunu yazma.

Elimde şu üst seviye komutlar var:
- forward <pwm>
- backward <pwm>
- left <pwm>
- right <pwm>
- stop

Motor kanalları:
- MOTOR_FL
- MOTOR_RL
- MOTOR_FR
- MOTOR_RR

Kurallar:
- forward <pwm>: tüm motorlar ileri
- backward <pwm>: tüm motorlar geri
- stop: tüm motorlar stop
- left <pwm>: sol motorlar ileri, sağ motorlar geri
- right <pwm>: sol motorlar geri, sağ motorlar ileri

F411 kartlarına gönderilecek komut formatı sade ve satır bazlı olsun.
Örnek formatlardan uygun olanı seç:
- SET FWD 120
- SET REV 120
- STOP

İstediklerim:
1. Üst seviye komutun 4 motora nasıl dağıtılacağını açıkla
2. Her komut için 4 motora gidecek örnek UART çıktısını ver
3. Kullanılacak veri yapıları nelerdir söyle
4. motor_router modülünün görevlerini açıkla
5. Son gönderilen komutların status için nasıl tutulacağını açıkla
6. Henüz tüm proje kodunu yazma
7. Sadece motor yönlendirme mantığına odaklan