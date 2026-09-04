# Rigicon Live

Yerel ağ üzerinde uçtan uca şifreli, kayıt tutmayan, anlık terminal mesajlaşma uygulaması. Bir telsiz gibi: aynı frekansa (porta) tutunan herkes birbirini duyar. Uygulama kapanınca hiçbir iz kalmaz.

---

## Özellikler

- **Uçtan uca şifreli** — ChaCha20-Poly1305 AEAD (RFC 8439). Paketler Wireshark ile bile açık metin olarak görünmez.
- **Sıfır kayıt** — dosya yok, log yok, cache yok, geçmiş yok. Kapatınca uçar.
- **Kanal = port** — 7444 varsayılan. Farklı sohbet grubu için farklı port. 7445'tekiler 7444'tekileri duymaz.
- **Aynı LAN'daki herkesi bulur** — UDP multicast (239.74.44.44), keşif ve mesajlaşma otomatik.
- **Cross-platform** — macOS (Intel + Apple Silicon), Windows, Linux. Tek kaynak kod.
- **Bağımlılık yok** — 100 KB civarı tek binary. Runtime, kütüphane, DLL gerekmez.
- **Dinamik ANSI renkler** — her katılımcıya rengi otomatik atanır.
- **OS bildirimleri** — terminal arka plandayken sistemin yerel bildirim sistemini tetikler.

---

## Hızlı Başlangıç (macOS)

1. [Releases](https://github.com/Quantre34/RigiconLive/releases/latest) sayfasından **`RigiconLive-universal`** dosyasını indir (Intel + Apple Silicon uyumlu, 118 KB).
2. Terminalde:
   ```bash
   chmod +x RigiconLive-universal
   ./RigiconLive-universal
   ```
3. Gatekeeper uyarı verirse: **Sistem Ayarları > Gizlilik ve Güvenlik** > "Yine de Aç" tıkla, sonra terminale dön ve tekrar çalıştır.

Karşılama ekranı sana üç şey sorar:

```
Rumuz / İsim giriniz: sahin
Kanal / Port [Varsayılan: 7444]:
Sistem bildirimleri alınsın mı? (e/H):
```

Enter'a basınca boş bırakılanlar için varsayılan kullanılır. İşte bu kadar. Aynı LAN'daki (aynı WiFi'daki) diğer Rigicon Live kullanıcılarını görürsün, yazışırsın.

---

## Windows

Windows kullanıcıları için iki yol var:

### Yol 1: İmzalı `.exe` indir (arkadaşın senden aldıysa)

`RigiconLive.exe` dosyasına çift tıkla. İmzalıysa doğrudan açılır. İmzasızsa **SmartScreen** uyarısı gelir → "Daha fazla bilgi" > "Yine de çalıştır".

### Yol 2: Kaynak koddan derle

Gereksinim: **MinGW-w64** (msys2 üzerinden gelir) veya **Visual Studio Build Tools** (MSVC).

```cmd
git clone https://github.com/Quantre34/RigiconLive.git
cd RigiconLive
platforms\windows\scripts\build.bat
```

Çıktı: `dist\windows\RigiconLive.exe`.

### İmzalama (opsiyonel ama SmartScreen için önerilir)

Kendi self-signed sertifikanla imzala:

```powershell
platforms\windows\scripts\generate-cert.ps1    # bir kereye mahsus; .pfx üretir
platforms\windows\scripts\sign-exe.ps1         # exe'yi imzalar
```

İmzadan sonra `.exe`'ye sağ tık > **Özellikler > Dijital İmzalar** sekmesinde "Rigicon Inc." görünür.

---

## Linux

```bash
git clone https://github.com/Quantre34/RigiconLive.git
cd RigiconLive
make
./dist/linux/RigiconLive
```

Gereksinim: `gcc` veya `clang`, `make`. `notify-send` yüklüyse OS bildirimleri de çalışır.

---

## Kullanım

Bağlandıktan sonra aşağıya doğru kayan bir sohbet ekranı görürsün:

```
═══════════════════════════════════════════════════════════
  R I G I C O N   L I V E   · Rigicon Inc.
═══════════════════════════════════════════════════════════
  Rumuz    : sahin
  Kanal    : 7444
  Bildirim : Açık
  Şifreleme: ChaCha20-Poly1305 (AEAD, RFC 8439)
  İz       : Sıfır. Kapanınca her şey gider.
═══════════════════════════════════════════════════════════
  Komutlar: /quit  /clear  /who  /help    Enter ile gönder
───────────────────────────────────────────────────────────

[14:03:22] * ayse kanala katıldı
[14:03:45] ayse: Merhaba
sahin ▸ Selam
```

Mesaj yazıp **Enter**'a basınca gönderilir. Herkes senin ismini kendine göre bir renkte görür.

### Komutlar

| Komut | Ne yapar |
|---|---|
| `/quit` | Kanaldan çıkar, uygulamayı kapatır (Ctrl+C ile aynı) |
| `/clear` | Ekranı temizler, banner'ı yeniden çizer |
| `/who` | Son 60 saniyede aktivite gösteren kullanıcıları listeler |
| `/help` | Komut listesini gösterir |
| `Ctrl+L` | `/clear` ile aynı |
| `Ctrl+C` | Çıkış |

### Kanal Değiştirme (Özel Grup)

Farklı bir sohbet grubu istiyorsan port değiştir. Örnek: pazarlama ekibi 7445'te, yazılım ekibi 7444'te:

```bash
./RigiconLive-universal --port 7445
```

Ya da başlangıçta port sorulduğunda `7445` yaz. Aynı porttakiler birbirini duyar, farklı porttakilerden habersizdirler.

### Komut Satırı Bayrakları

```
Kullanım: RigiconLive [--nick <isim>] [--port <numara>]

  -n, --nick <isim>    Başlangıç sorusunu atla, direkt bu ismi kullan
  -p, --port <numara>  Port sorusunu atla, direkt bu porta bağlan
  -v, --version        Sürüm bilgisi
  -h, --help           Yardım
```

Örnek — sık kullandığın port ve rumuz için alias:

```bash
alias chat='~/bin/RigiconLive-universal --nick sahin --port 7444'
```

---

## Nasıl Çalışır?

### Ağ

- **UDP multicast** grubu `239.74.44.44`, TTL 4 (yerel ağdan çıkmaz).
- Kanal = port. Bind edilen port aynı olan herkes aynı gruba katılır.
- Aynı hosttaki iki instance de birbirini duyar (`IP_MULTICAST_LOOP=1`).
- Anahtar keşif protokolü yok — sadece bağlan ve konuş.

### Şifreleme

- **ChaCha20-Poly1305** — RFC 8439 AEAD şeması.
- Anahtar binary'e gömülü (256-bit). Rigicon Live çalıştıran her cihaz aynı anahtara sahip. Ağdaki bir gözlemci (Wireshark, port mirror, vs.) paket içeriğini okuyamaz.
- Her paket rastgele 96-bit nonce ile mühürlenir. Poly1305 MAC her mesajın bütünlüğünü doğrular — kurcalanan paketler reddedilir.
- Uygulama açılırken kripto self-test koşar (RFC 8439 test vektörü). Fail ederse başlamaz.

### Kayıt Tutmama

- Hiçbir dosya oluşturulmaz — ne log, ne db, ne cache.
- Konuşma geçmişi sadece RAM'de yaşar. Uygulama kapandığı an OS onu recycle eder.
- Terminal ekranındaki geçmiş, terminalin scrollback tamponunda — istersen terminali kapat, o da gider.

---

## Sık Sorulan Sorular

**S: İnternet üzerinden çalışır mı?**  
Hayır. Multicast internet üzerinden yönlendirilmez. Uzaktaki arkadaşlarla kullanmak istersen Tailscale / ZeroTier / WireGuard gibi VPN kurup herkesi aynı sanal LAN'a bağla — sonra sıfır değişiklikle çalışır.

**S: Kaç kişi olabilir?**  
LAN'ın taşıyabildiği kadar. Multicast band genişliği ~250 kişiye kadar rahat.

**S: Birden fazla kanal aynı anda dinlenebilir mi?**  
Hayır, tek instance tek porta bağlanır. İki farklı kanalı takip etmek istersen iki terminal aç, ikisinde de `--port` farklı olarak çalıştır.

**S: Kablosuz WiFi'da multicast bazen düşmüyor mu?**  
Bazı router'lar multicast paketleri yavaş/güvenilmez şekilde iletir (WiFi standardına gömülü bir limitasyon). Sohbet aksarsa router'ın "IGMP snooping" ayarını kapat ya da kablolu bağlantı dene.

**S: macOS "hasarlı" diyor, açtırmıyor.**  
Gatekeeper karantinası. Bir kez şu komutla temizle:
```bash
xattr -d com.apple.quarantine ./RigiconLive-universal
```
Sonra normal şekilde çalıştır.

**S: Port 7444 dolu diyor.**  
Başka bir uygulama o portu tutuyor. Farklı bir port seç — 7445, 7500, ne olursa.

**S: Bildirim gelmedi.**  
- macOS: Sistem Ayarları > Bildirimler > Script Editor / Terminal iznini kontrol et.
- Linux: `notify-send` kurulu mu? `sudo apt install libnotify-bin`.
- Windows: PowerShell'in kısıtlı politika modunda olmadığından emin ol.

---

## Kaynak Koddan Derleme

Tek komut yeter:

```bash
# macOS / Linux
make

# macOS universal binary (Intel + Apple Silicon tek dosya)
cc -O2 -Wall -std=c99 -D_DARWIN_C_SOURCE -arch arm64 -arch x86_64 \
   -o dist/macos/RigiconLive-universal \
   src/main.c src/crypto.c src/net.c src/term.c src/notify.c -lpthread

# Windows
platforms\windows\scripts\build.bat
```

Kaynak yapısı:

```
src/
├── main.c        Giriş noktası, başlangıç akışı, thread yönetimi
├── crypto.c/h    ChaCha20-Poly1305 AEAD (elle, RFC 8439)
├── net.c/h       UDP multicast (Winsock2 / POSIX abstraction)
├── term.c/h      Terminal, raw mode, ANSI paleti
├── notify.c/h    OS bildirim (PowerShell / osascript / notify-send)
└── rgcn.h        Ortak sabitler
```

Toplam ~1560 satır C99. Harici bağımlılık yok.

---

## Güvenlik Notları

- **Kapalı sistem güven modeli:** Anahtar binary'e gömülü. Bu, Rigicon Live paketlerini ağdaki bir gözlemciden korur. Binary'ye erişimi olan biri anahtarı çıkarabilir — dolayısıyla bu app "tanıdıklar arası özel kanal" için tasarlandı, "devlet düşmanına karşı" değil.
- **Ephemeral tasarım:** Kod diske hiçbir şey yazmaz. Ancak terminal kendisi scrollback tutabilir. Paranoyakysan terminalini `⌘K` ile temizle veya "clear scrollback" ayarını aç.
- **Kimlik doğrulama yok:** Rumuz taklit edilebilir. Bu bir "gizli servis kripto telefonu" değil, ofis arkadaşları arasında hızlı ve gürültüsüz bir konuşma aracı.
- **Multicast sızıntısı:** Rigicon Live paketleri yerel ağdaki her cihaza gider. Aynı ağda Rigicon Live çalıştırmayan biri paketleri görebilir ama içeriklerini çözemez (şifreli).

---

## Sürüm

- **1.0.0** — İlk sürüm. Temel özellik seti tamam.

## Lisans

Bu deponun sahibi Rigicon Inc.'e aittir. Dağıtım ve kullanım hakları depo sahibinin takdirindedir.
