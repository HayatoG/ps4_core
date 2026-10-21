#!/bin/bash
# ftp_mirror.sh [remote_dir] [local_dir] : recursive resumable FTP mirror from GoldHEN's FTP server,
# 3 parallel transfers. Defaults: the running game's app0 (/mnt/sandbox/pfsmnt/CUSA00223-app0) into dump/.
# The update: ftp_mirror.sh /mnt/sandbox/pfsmnt/CUSA00223-patch0 dump/patch0, then
# scripts/make_game_view.py dump dump/patch0 game/CUSA00223. eboot.bin and sce_module/*.prx are skipped
# (take them from the Itemzflow dump). PS4_FTP: server (default ftp://192.168.1.10:2121); TITLE_ID.
H=${PS4_FTP:-ftp://192.168.1.10:2121}; T=${TITLE_ID:-CUSA00223}
list() { # $1 remote dir -> prints "type size path"
  curl -s --max-time 30 "$H$1/" | while read -r perm _ _ _ size _ _ _ name; do
    [ "$name" = . ] || [ "$name" = .. ] || [ -z "$name" ] && continue
    if [ "${perm:0:1}" = d ]; then list "$1/$name"; else echo "$size $1/$name"; fi
  done
}
R=${1:-/mnt/sandbox/pfsmnt/$T-app0}; L=${2:-$(cd -- "$(dirname -- "$0")/.." && pwd)/dump}; mkdir -p "$L"
list "$R" | grep -vE " $R/(eboot\.bin|sce_module/.*\.prx)$" > "$L/.filelist"
echo "LIST $(wc -l < "$L/.filelist") files, $(awk '{s+=$1} END{printf "%.1f GB", s/1e9}' "$L/.filelist")"
fetch() { size=$1; rp=$2; lp="$L/${rp#$R/}"; mkdir -p "$(dirname "$lp")"
  for t in 1 2 3 4 5 6; do curl -s -C - -o "$lp" "$H$rp"; rc=$?; [ $rc -eq 0 ] || [ $rc -eq 33 ] && break; sleep 5; done
  echo "DONE ${rp#$R/} $(stat -c %s "$lp")"; }
export -f fetch; export H R L
sort -n "$L/.filelist" | xargs -P3 -L1 bash -c 'fetch "$0" "$1"'
echo "ALLDONE $R"
