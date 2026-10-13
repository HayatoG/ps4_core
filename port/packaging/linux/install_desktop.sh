#!/usr/bin/env bash
# Adds the game to the applications menu, pointing at this folder (--remove undoes it).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
source "$here/port.env" 2>/dev/null || { PORT_NAME="inFAMOUS Second Son"; PORT_SLUG=infamous; PORT_ICON=infamous; }
entry=$apps/$PORT_SLUG-pc.desktop
if [[ ${1:-} == --remove ]]; then
    rm -f "$entry"; echo "Removed $entry"; exit 0
fi
mkdir -p "$apps"
cat > "$entry" <<DESKTOP
[Desktop Entry]
Type=Application
Name=$PORT_NAME
Comment=$PORT_NAME (PC port)
Exec="$here/$PORT_NAME"
Path=$here
Icon=$here/launcher/$PORT_ICON.png
Terminal=false
Categories=Game;
StartupWMClass=$PORT_SLUG-pc
DESKTOP
echo "Installed $entry"
