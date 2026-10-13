#!/usr/bin/env bash
# Stops packaging when the staged folder holds a file that must not ship: game files, console
# firmware, official art, or anything a player made (saves, caches, settings, logs).
set -euo pipefail
dest=$1
forbidden=$(find "$dest" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname savedata \
    -o -iname '*.psarc' -o -iname '*.sprx' -o -iname '*.prx' -o -iname '*.pkg' -o -iname '*.sfo' \
    -o -iname icon0.png -o -iname 'pic0.png' -o -iname 'pic1.png' -o -iname 'playgo-chunk*' \
    -o -iname boot-linked.bin -o -iname content.bin -o -iname tlg.ini -o -iname settings.json \
    -o -iname '*.log' -o -iname '*.spv' -o -path '*/user/*' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi
echo "Package check: no game, firmware or player files."
