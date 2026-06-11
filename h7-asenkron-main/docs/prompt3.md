Şimdi sadece terminalden gelen komutların parser tasarımını yap. Henüz tüm projeyi yazma, sadece parser katmanına odaklan.

Komutlar:
- forward <pwm>
- backward <pwm>
- left <pwm>
- right <pwm>
- stop
- status
- help

Kurallar:
- PWM değeri hareket komutunun içinde olacak
- Ayrı pwm komutu olmayacak
- PWM değeri 0-255 aralığında olacak
- forward 120 geçerli
- forward geçersiz
- forward abc geçersiz
- forward 999 geçersiz

İstediklerim:
1. Parser’ın nasıl çalışacağını açıkla
2. Komutları nasıl tokenize edeceğini anlat
3. Hangi enum/struct yapılarının gerekli olduğunu söyle
4. Hata durumlarını listele
5. Geçerli ve geçersiz giriş örnekleri ver
6. Komut parse sonucunda nasıl bir veri yapısı üretileceğini tasarla
7. İstersen header/source için önerilen arayüzleri yaz ama henüz tam proje kodunu yazma
8. Cevabı sadece parser tasarımıyla sınırla