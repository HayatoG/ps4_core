#!/usr/bin/env bash
# Instala o atalho do launcher no menu de aplicativos (Linux, padrão freedesktop: GNOME, KDE...).
# Nome, app id e ícone vêm de port/packaging/port.env; o ícone é o do port
# (port/packaging/icon/<slug>-icon-1024.png, arte original do projeto).
# Uso: scripts/install_desktop.sh [--remove]
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
# shellcheck source=../port/packaging/port.env
source "$root/port/packaging/port.env"
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
icons=${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor
icon=$PORT_SLUG-pc
desktop=$apps/$icon.desktop
if [[ ${1:-} == --remove ]]; then
    rm -f "$desktop"; find "$icons" -name "$icon.png" -delete 2>/dev/null || true
    echo "Atalho removido."; exit 0
fi
icon_src="$root/port/packaging/icon/$PORT_SLUG-icon-1024.png"
for size in 512 256 128 64 48; do
    mkdir -p "$icons/${size}x${size}/apps"
    python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).resize((int(sys.argv[3]),) * 2, Image.LANCZOS).save(sys.argv[2])" \
        "$icon_src" "$icons/${size}x${size}/apps/$icon.png" "$size" 2>/dev/null ||
        cp "$icon_src" "$icons/${size}x${size}/apps/$icon.png"
done
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$icons" 2>/dev/null || true
mkdir -p "$apps"
cat > "$desktop" <<DESK
[Desktop Entry]
Type=Application
Name=$PORT_NAME
Comment=Native PC port launcher (your own PS4 dump)
Comment[pt_BR]=Launcher do port nativo para PC (com o seu dump do PS4)
Exec=$root/${PORT_SLUG}_launcher.sh
Path=$root
Icon=$icon
Terminal=false
Categories=Game;
StartupWMClass=$icon
DESK
command -v update-desktop-database >/dev/null && update-desktop-database -q "$apps" 2>/dev/null || true
echo "Atalho instalado: $desktop"
