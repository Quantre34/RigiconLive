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

## Kurulum

### Tek Satır (önerilen)

Terminalde `RigiconLive` yazınca doğrudan açılsın istiyorsan tek komut yeter — indirir, kurar, PATH'e ekler:

**macOS / Linux:**
```bash
curl -fsSL https://github.com/Quantre34/RigiconLive/raw/main/install.sh | sh
```

**Windows:**

SmartScreen "unwanted software" uyarısı için önce **Rigicon Inc. sertifikasını** kur, sonra binary'yi indir. İki yol:

**Yol A — Çift tıkla ile kur (en kolay):**

1. **[RigiconInc.cer](https://github.com/Quantre34/RigiconLive/raw/main/certs/RigiconInc.cer)** dosyasını indir *(link'e sağ tık → "Bağlantıyı farklı kaydet")*.
2. İndirilen `RigiconInc.cer`'e **çift tıkla** → Windows Sertifika Görüntüleyici açılır.
3. **"Sertifikayı Yükle..."** butonuna bas.
4. **Mağaza Konumu:** "Geçerli Kullanıcı" seç → İleri.
5. **"Tüm sertifikaları aşağıdaki depoya yerleştir"** seç → "Gözat" → **"Güvenilen Kök Sertifika Yetkilileri"** seç → İleri → Son.
6. Güvenlik uyarısı gelirse **Evet** de.
7. Aynı `.cer`'e tekrar çift tıkla, aynı adımları yap ama bu sefer **"Güvenilen Yayımcılar"** deposunu seç.
8. Şimdi `RigiconLive.exe`'yi indir → SmartScreen uyarısı olmadan açılır.

**Yol B — PowerShell tek satır (daha hızlı):**

```powershell
# 1) Bir kereye mahsus: Rigicon Inc. sertifikasını Trusted Publisher olarak yükle
iwr -useb https://github.com/Quantre34/RigiconLive/raw/main/certs/install-cert.ps1 | iex

# 2) Binary'yi indir ve PATH'e ekle
iwr -useb https://github.com/Quantre34/RigiconLive/raw/main/install.ps1 | iex
```

Her iki yol da admin gerektirmez (CurrentUser store'a yazar), sadece o hesap için geçerlidir.

Kurulum bittiğinde yeni bir terminal aç ve yaz:

```
RigiconLive
```

İşte bu kadar. Kurulum ne yapıyor:
- **macOS/Linux:** Binary'yi `/usr/local/bin/RigiconLive`'a koyar (sudo yoksa `~/.local/bin`'e ve `.zshrc`/`.bashrc` dosyasına PATH satırı ekler).
- **Windows:** `%LOCALAPPDATA%\Programs\RigiconLive\RigiconLive.exe`'ye kopyalar ve kullanıcı PATH'ine ekler (admin gerekmez).

### Güncelleme

Yeni sürüm çıktığında **aynı kurulum komutunu tekrar çalıştır** — mevcut binary'nin üstüne yazar, PATH satırı zaten varsa tekrar eklenmez:

```bash
# macOS/Linux
curl -fsSL https://github.com/Quantre34/RigiconLive/raw/main/install.sh | sh

# Windows (PowerShell)
iwr -useb https://github.com/Quantre34/RigiconLive/raw/main/install.ps1 | iex
```

### Kaldırma (Uninstall)

Tek satır kaldırma — kurulumdan geriye hiçbir şey bırakmaz:

```bash
# macOS / Linux
curl -fsSL https://github.com/Quantre34/RigiconLive/raw/main/uninstall.sh | sh
```

```powershell
# Windows (PowerShell)
iwr -useb https://github.com/Quantre34/RigiconLive/raw/main/uninstall.ps1 | iex
```

**Ne temizleniyor:**

| Platform | Binary | PATH | Sertifika |
|---|---|---|---|
| macOS/Linux | `~/.local/bin/RigiconLive` **veya** `/usr/local/bin/RigiconLive` | `~/.zshrc` / `~/.bashrc` / `~/.config/fish/config.fish` içindeki `# Rigicon Live` yorumu + altındaki `export PATH=...` satırı | *(macOS'ta sertifika kurulmuyor)* |
| Windows | `%LOCALAPPDATA%\Programs\RigiconLive\` klasörü tümüyle | User PATH'inden `RigiconLive` içeren giriş | `Cert:\CurrentUser\Root` ve `\TrustedPublisher`'daki `Rigicon Inc.` sertifikası (soru sorar) |

Kaldırdıktan sonra **yeni bir terminal** aç, `RigiconLive` yaz — "command not found" görmelisin. Görüyorsan başarılı.

#### Manuel Kaldırma — macOS / Linux

Bilgisayarında ne olduğunu kendi gözünle görüp elden silmek istersen:

```bash
# 1. Binary'lerin nerede olduğunu bul
which -a RigiconLive
ls -la /usr/local/bin/RigiconLive ~/.local/bin/RigiconLive 2>/dev/null

# 2. Binary'yi sil (yazma iznine göre sudo gerekebilir)
rm -f ~/.local/bin/RigiconLive
sudo rm -f /usr/local/bin/RigiconLive       # root olarak kurduysan

# 3. Shell rc dosyasından PATH satırını temizle
#    (aşağıdakini kendi shell'ine göre seç: .zshrc / .bashrc / config.fish)
# .zshrc / .bashrc icin: "# Rigicon Live" satırını ve altındaki export'u sil
nano ~/.zshrc
# ...içinde şu iki satırı bul ve sil:
#   # Rigicon Live
#   export PATH="$HOME/.local/bin:$PATH"

# 4. Yeni bir terminal ac, dogrula:
which RigiconLive        # ciktisi bos olmali
```

#### Manuel Kaldırma — Windows

```powershell
# 1. Kurulu binary klasorunu sil
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\Programs\RigiconLive"

# 2. PATH'ten cikar
$p = [Environment]::GetEnvironmentVariable("Path", "User")
$new = ($p -split ";") | Where-Object { $_ -notmatch "RigiconLive" }
[Environment]::SetEnvironmentVariable("Path", ($new -join ";"), "User")

# 3. (Opsiyonel) Rigicon Inc. sertifikasini kaldir
Get-ChildItem Cert:\CurrentUser\Root, Cert:\CurrentUser\TrustedPublisher |
    Where-Object { $_.Subject -match "Rigicon Inc\." } | Remove-Item

# 4. Yeni bir PowerShell ac, dogrula:
Get-Command RigiconLive          # "cannot find" hatasi almalısın
```

GUI ile PATH temizlemek istersen:
1. Başlat > **"environment variables"** yaz > "Kullanıcı için ortam değişkenlerini düzenle"
2. Üst kutuda **Path**'i seç > **Düzenle...**
3. `RigiconLive` içeren satırı bul > **Sil** > Tamam
4. Açık PowerShell/CMD'yi kapat, yeni aç → PATH güncellendi

Sertifikayı GUI ile kaldırmak istersen: **certmgr.msc** çalıştır > "Güvenilen Kök Sertifika Yetkilileri" ve "Güvenilen Yayımcılar" altında "Rigicon Inc."'i bul, sağ tık > Sil.

#### Sorun Giderme

**"Permission denied" hatası alıyorsam:** Binary sudo ile kurulmuş demektir (`/usr/local/bin/`). Silmek için `sudo rm /usr/local/bin/RigiconLive`.

**Uninstall script çalıştı ama `RigiconLive` hâlâ tanınıyor:** Muhtemelen açık terminalinin PATH'i eski. Bir yeni terminal aç ve tekrar dene.

**Kurulumdan hiçbir iz kalmasın istiyorum:**
- macOS'ta uygulama hiçbir dosya oluşturmuyor (bakış açısı olarak "sıfır kayıt" tasarımı). Sadece binary + `.zshrc` satırı temizlenir; başka hiçbir şey yok.
- Windows'ta aynı — `%LOCALAPPDATA%\Programs\RigiconLive` klasörü + PATH + sertifika dışında hiçbir dosya, log, registry anahtarı yok.

### Manuel İndirme

Kurulum scriptini kullanmak istemiyorsan, [Releases](https://github.com/Quantre34/RigiconLive/releases/latest) sayfasından platformuna uygun dosyayı indir:

| Platform | Dosya | Çalıştırma |
|---|---|---|
| macOS (Intel + Apple Silicon) | `RigiconLive-macos` | `chmod +x RigiconLive-macos && ./RigiconLive-macos` |
| Linux (x86_64) | `RigiconLive-linux` | `chmod +x RigiconLive-linux && ./RigiconLive-linux` |
| Windows (x64) | `RigiconLive.exe` | Çift tıkla veya CMD/PowerShell'de çalıştır |

**macOS notu:** Gatekeeper uyarı verirse: **Sistem Ayarları > Gizlilik ve Güvenlik** > "Yine de Aç". Ya da:
```bash
xattr -d com.apple.quarantine ./RigiconLive-macos
```

**Windows notu:** SmartScreen "unwanted software / bilinmeyen yayımcı" diyorsa yukarıdaki [Windows Sertifika Kurulumu](#kurulum) adımlarını uygula. Anlık geçmek istersen "Daha fazla bilgi" > "Yine de çalıştır" tıklayabilirsin ama her indirmede uyarı gelir.

---

## Windows Sertifika Kurulumu

Kurulum bölümündeki Yol A / Yol B ile aynıdır. Detaylı ekran görüntüleri için [kurulum bölümü](#kurulum) sayfasının yukarısına bak.

---

## İlk Çalıştırma

Karşılama ekranı sana üç şey sorar:

```
Rumuz / İsim giriniz: sahin
Kanal / Port [Varsayılan: 7444]:
Sistem bildirimleri alınsın mı? (e/H):
```

Enter'a basınca boş bırakılanlar için varsayılan kullanılır. İşte bu kadar. Aynı LAN'daki (aynı WiFi'daki) diğer Rigicon Live kullanıcılarını görürsün, yazışırsın.

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
| `/who` | Son 90 saniyede aktivite gösteren kullanıcıları listeler |
| `/status` | Bağlantı durumu — kendi station ID'in + her peer'ın IP:port'u, son aktivite süresi, unicast/broadcast durumu |
| `/help` | Komut listesini gösterir |
| `Ctrl+L` | `/clear` ile aynı |
| `Ctrl+C` | Çıkış |

### Kanal Değiştirme (Özel Grup)

Farklı bir sohbet grubu istiyorsan port değiştir. Örnek: pazarlama ekibi 7445'te, yazılım ekibi 7444'te:

```bash
RigiconLive --port 7445
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
alias chat='RigiconLive --nick sahin --port 7444'
```

---

## Nasıl Çalışır?

### Ağ — Üç Katmanlı Dağıtım

Rigicon Live her paketi üç farklı yolla iletir. Amaç: WiFi'nin her türlü kötü davranışına dayanıklı olmak.

1. **UDP multicast** — grup `239.74.44.44`, TTL 4. Modern anahtarlarda ideal, aynı hosttaki iki instance de birbirini duyar (`IP_MULTICAST_LOOP=1`).
2. **Broadcast** — hem limited (`255.255.255.255`) hem de her interface için subnet-directed. Multicast'i filtreleyen ucuz WiFi router'lar için sigorta.
3. **Unicast peer-cache** — bir peer'ın IP'sini bir kez öğrendik mi (herhangi bir alınan paketten), sonraki mesajlar direkt o cihaza da atılır. WiFi'de unicast **ACK + retransmit** var → paket kaybına karşı bağışık. `nc` neden çalışıyorsa bu da o yüzden çalışır.

Kanal = port. Bind edilen port aynı olan herkes aynı sohbete katılır. Yeni bir peer katıldığında karşılıklı JOIN alışverişi ile iki taraf da birbirini "gördü" olur; ondan sonra tüm konuşma unicast üzerinden akar. Bir cihaz kanaldan ayrılırsa peer cache 5 dakikada temizlenir.

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
Evet, WiFi'de multicast/broadcast ACK'sız ve en düşük hızda iletilir (bandwidth tasarrufu için) — kayıp oranı %30-70 olabilir. Rigicon Live bunu çözer: peer'ın IP'si bir kez öğrenildikten sonra mesajlar **unicast** olarak da atılır (WiFi'de unicast ACK + retransmit ile güvenilir). Yani ilk JOIN/keşif paketi yeterse, sonrası pürüzsüz akar.

**S: JOIN geldi ama sonrasında hiçbir mesaj gelmiyor. Ne oluyor?**  
Eski sürümlerde (v1.0.3 ve öncesi) WiFi multicast paket kaybına takılan bilinen bir sorundu. **v1.0.4+**'ta hybrid multicast+broadcast, **v1.0.5+**'ta unicast peer-cache ile çözüldü. İlk yapılacak: en son sürüme güncelle:
```bash
# macOS/Linux
curl -fsSL https://github.com/Quantre34/RigiconLive/raw/main/install.sh | sh
# Windows
iwr -useb https://github.com/Quantre34/RigiconLive/raw/main/install.ps1 | iex
```
Yine çalışmazsa: (1) macOS Sistem Ayarları > Ağ > Güvenlik Duvarı'nda RigiconLive'ı izin ver, (2) Windows'ta ilk çalıştırmada gelen firewall isteğine "Özel Ağlar" onay ver, (3) `ping <arkadaşın-ip>` çalışıyor mu kontrol et — çalışmıyorsa AP isolation / VLAN engeli var.

**S: macOS "hasarlı" diyor, açtırmıyor.**  
Gatekeeper karantinası. Bir kez şu komutla temizle:
```bash
xattr -d com.apple.quarantine $(which RigiconLive)
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

Sadece kendin geliştirmek/incelemek istiyorsan. Normal kullanım için yukarıdaki tek satır kurulum yeterli.

```bash
git clone https://github.com/Quantre34/RigiconLive.git
cd RigiconLive

# macOS / Linux
make

# macOS universal binary (Intel + Apple Silicon tek dosya)
cc -O2 -Wall -std=c99 -D_DARWIN_C_SOURCE -arch arm64 -arch x86_64 \
   -o dist/macos/RigiconLive-universal \
   src/main.c src/crypto.c src/net.c src/term.c src/notify.c -lpthread

# Windows
platforms\windows\scripts\build.bat
```

### Windows kod imzalama (opsiyonel)

Kendi self-signed sertifikanla `.exe`'yi imzalayabilirsin. SmartScreen uyarısını "trusted publisher" olduğun makinelerde kaldırır:

```powershell
platforms\windows\scripts\generate-cert.ps1    # bir kereye mahsus; .pfx üretir
platforms\windows\scripts\sign-exe.ps1         # exe'yi imzalar
```

İmzadan sonra `.exe`'ye sağ tık > **Özellikler > Dijital İmzalar** sekmesinde "Rigicon Inc." görünür.

### Kaynak yapısı

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

### Otomatik release (GitHub Actions)

Repo bir CI/CD içeriyor (`.github/workflows/release.yml`). Yeni sürüm yayınlamak için:

```bash
git tag v1.0.2
git push --tags
```

3-4 dakika içinde 3 platform (macOS / Linux / Windows) için binary derlenip Releases sayfasına eklenir.

---

## Güvenlik Notları

- **Kapalı sistem güven modeli:** Anahtar binary'e gömülü. Bu, Rigicon Live paketlerini ağdaki bir gözlemciden korur. Binary'ye erişimi olan biri anahtarı çıkarabilir — dolayısıyla bu app "tanıdıklar arası özel kanal" için tasarlandı, "devlet düşmanına karşı" değil.
- **Ephemeral tasarım:** Kod diske hiçbir şey yazmaz. Ancak terminal kendisi scrollback tutabilir. Paranoyakysan terminalini `⌘K` ile temizle veya "clear scrollback" ayarını aç.
- **Kimlik doğrulama yok:** Rumuz taklit edilebilir. Bu bir "gizli servis kripto telefonu" değil, ofis arkadaşları arasında hızlı ve gürültüsüz bir konuşma aracı.
- **Multicast sızıntısı:** Rigicon Live paketleri yerel ağdaki her cihaza gider. Aynı ağda Rigicon Live çalıştırmayan biri paketleri görebilir ama içeriklerini çözemez (şifreli).

---

## Sürüm

- **1.0.6** — Rejoin bug fix: peer'ı (nick + station_id) ile takip et, session değişince yeni say. Her 30s HELLO heartbeat. 90s inaktif peer otomatik "ayrıldı". Yeni `/status` komutu (peer'ların IP/port/latency).
- **1.0.5** — Peer-cache unicast: bir cihazın IP'si öğrenildikten sonra mesajlar unicast olarak da atılır (WiFi multicast paket kaybını çözer).
- **1.0.4** — Hybrid multicast + broadcast: multicast'i filtreleyen router'lar için broadcast fallback.
- **1.0.3** — Windows kod imzalama (`RigiconInc.cer` + otomatik CI imzalama).
- **1.0.2** — GitHub Releases'e sertifika dosyası dahil edildi.
- **1.0.1** — Tek satır kurulum scriptleri, uninstall scriptleri, GitHub Actions ile 3 platform için otomatik build.
- **1.0.0** — İlk sürüm. Temel özellik seti tamam.

## Lisans

Bu deponun sahibi Rigicon Inc.'e aittir. Dağıtım ve kullanım hakları depo sahibinin takdirindedir.
