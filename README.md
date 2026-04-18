# 🛡️ NexMatch Anti-Cheat (Client)

NexMatch turnuva platformu için geliştirilmiş, şeffaflık odaklı anti-cheat yazılımı.

## ✨ Şeffaflık ve Güvenlik
* **Açık Kaynak:** Kullanıcı arayüzü ve temel istemci mantığı (NexMatch_Public.cpp) tamamen incelenebilir durumdadır. Bilgisayarınızda gizli bir işlem yapılmadığını görebilirsiniz.
* **Kapalı Modül:** Hilecilerin sistemi manipüle etmesini önlemek adına, sadece 'tespit algoritmaları' (Core) kapalı modül olarak tutulmaktadır.
* **Microsoft Onaylı:** Yazılımımız Microsoft Malware Security ekibi tarafından incelenmiş ve **"Clean"** (Zararsız) olarak raporlanmıştır.

## ⚙️ Nasıl Çalışır?
1. Oyun sırasında rastgele aralıklarla ekran bilgileri alır.
2. Steam ID ve oyun süreçlerini doğrular.
3. Alınan verileri SSL/TLS güvenliği ile NexMatch sunucularına iletir.

## 📄 Lisans
Bu projenin halka açık kısımları **MIT Lisansı** ile korunmaktadır. Çekirdek algoritmalar NexMatch'e ait özel mülkiyettir.
