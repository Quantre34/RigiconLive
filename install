#!/usr/bin/env sh
# Rigicon Live - one-shot installer for macOS and Linux.
#
# Kullanim:
#   curl -fsSL https://github.com/Quantre34/RigiconLive/raw/main/install.sh | sh
#
# Ne yapar:
#   1. GitHub Releases'ten en son binary'yi indirir (macOS universal veya Linux x86_64).
#   2. /usr/local/bin/RigiconLive'a kopyalar (sudo gerekirse ister).
#   3. Yoksa ~/.local/bin'e kurar ve PATH'e ekler.
#
# Sonuc: Terminalde "RigiconLive" yazinca acilir.

set -eu

REPO="Quantre34/RigiconLive"
BIN_NAME="RigiconLive"

OS="$(uname -s)"
case "$OS" in
    Darwin) ASSET="RigiconLive-macos" ;;
    Linux)  ASSET="RigiconLive-linux" ;;
    *) echo "Desteklenmeyen isletim sistemi: $OS" >&2; exit 1 ;;
esac

echo "==> $OS icin en son surum aranıyor..."
LATEST_URL="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
    | grep -oE '"browser_download_url": *"[^"]*'"$ASSET"'"' \
    | head -1 \
    | sed 's/.*"browser_download_url": *"//;s/"$//')"

if [ -z "${LATEST_URL:-}" ]; then
    echo "Hata: Son surumde $ASSET bulunamadi." >&2
    echo "       https://github.com/${REPO}/releases sayfasindan manuel indirebilirsin." >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Indiriliyor: $LATEST_URL"
curl -fsSL "$LATEST_URL" -o "$TMP/$BIN_NAME"
chmod +x "$TMP/$BIN_NAME"

# Once /usr/local/bin dene (klasik yer, PATH'te olur)
DEST_DIR=""
if [ -w "/usr/local/bin" ]; then
    DEST_DIR="/usr/local/bin"
elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    DEST_DIR="/usr/local/bin"
    NEED_SUDO=1
elif [ -t 0 ] && command -v sudo >/dev/null 2>&1; then
    DEST_DIR="/usr/local/bin"
    NEED_SUDO=1
fi

if [ -z "$DEST_DIR" ]; then
    # sudo yok - kullanici dizinine kur
    DEST_DIR="$HOME/.local/bin"
    mkdir -p "$DEST_DIR"
fi

echo "==> Kuruluyor: $DEST_DIR/$BIN_NAME"
if [ "${NEED_SUDO:-0}" = "1" ]; then
    sudo install -m 755 "$TMP/$BIN_NAME" "$DEST_DIR/$BIN_NAME"
else
    install -m 755 "$TMP/$BIN_NAME" "$DEST_DIR/$BIN_NAME"
fi

# PATH kontrolu
case ":${PATH}:" in
    *":${DEST_DIR}:"*) IN_PATH=1 ;;
    *)                 IN_PATH=0 ;;
esac

if [ "$IN_PATH" = "0" ]; then
    # ~/.local/bin PATH'te degil - shell rc'sine ekle
    SHELL_NAME="$(basename "${SHELL:-sh}")"
    case "$SHELL_NAME" in
        zsh)  RC="$HOME/.zshrc"  ;;
        bash) RC="$HOME/.bashrc" ;;
        fish) RC="$HOME/.config/fish/config.fish" ;;
        *)    RC="" ;;
    esac
    if [ -n "$RC" ] && [ -w "$(dirname "$RC")" ]; then
        LINE='export PATH="$HOME/.local/bin:$PATH"'
        if [ "$SHELL_NAME" = "fish" ]; then
            LINE='set -gx PATH $HOME/.local/bin $PATH'
        fi
        if ! grep -qsF "$DEST_DIR" "$RC" 2>/dev/null; then
            printf '\n# Rigicon Live\n%s\n' "$LINE" >> "$RC"
            echo "==> PATH'e eklendi: $RC"
            echo "    Yeni terminal ac ya da 'source $RC' calistir."
        fi
    fi
fi

echo ""
echo "[+] Kuruldu: $DEST_DIR/$BIN_NAME"
echo ""
if [ "$IN_PATH" = "1" ]; then
    echo "Simdi terminalde su komutu yaz:"
    echo "    RigiconLive"
else
    echo "Yeni terminal ac, ardindan:"
    echo "    RigiconLive"
fi
