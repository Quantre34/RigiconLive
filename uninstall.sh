#!/usr/bin/env sh
# Rigicon Live - kaldirma scripti (macOS ve Linux).
#
# Kullanim:
#   curl -fsSL https://github.com/Quantre34/RigiconLive/raw/main/uninstall.sh | sh
#
# Ne yapar:
#   1. /usr/local/bin/RigiconLive ve ~/.local/bin/RigiconLive dosyalarini siler.
#   2. .zshrc / .bashrc / config.fish dosyasindan "Rigicon Live" PATH satirini kaldirir.
#
# Not: PATH degisikligi mevcut terminalde etkili degildir - yeni terminal ac.

set -eu

BIN_NAME="RigiconLive"
REMOVED=0

# 1) Binary'leri sil
for DIR in /usr/local/bin "$HOME/.local/bin"; do
    P="$DIR/$BIN_NAME"
    if [ -e "$P" ]; then
        if [ -w "$P" ] || [ -w "$DIR" ]; then
            rm -f "$P"
        else
            sudo rm -f "$P"
        fi
        echo "[-] Silindi: $P"
        REMOVED=1
    fi
done

# 2) Shell rc dosyalarindan PATH satirini kaldir
for RC in "$HOME/.zshrc" "$HOME/.bashrc" "$HOME/.config/fish/config.fish"; do
    [ -f "$RC" ] || continue
    if grep -q "Rigicon Live" "$RC" 2>/dev/null; then
        # 2 satirlik blogu kaldir: "# Rigicon Live" ve altindaki export/set satiri
        tmp="$(mktemp)"
        awk '
            /^# Rigicon Live$/ { skip=2; next }
            skip > 0           { skip--; next }
            { print }
        ' "$RC" > "$tmp"
        mv "$tmp" "$RC"
        echo "[-] PATH satiri temizlendi: $RC"
        REMOVED=1
    fi
done

if [ "$REMOVED" = "0" ]; then
    echo "Rigicon Live zaten kurulu degil."
else
    echo ""
    echo "[+] Kaldirildi."
    echo "    Yeni bir terminal acinca PATH temizlenmis olur."
fi
