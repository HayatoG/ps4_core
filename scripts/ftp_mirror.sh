#!/bin/bash
# mirror.sh <remote_dir> <local_dir> : recursive resumable FTP mirror, 3 parallel transfers
H=ftp://192.168.1.10:2121
list() { # $1 remote dir -> prints "type size path"
  curl -s --max-time 30 "$H$1/" | while read -r perm _ _ _ size _ _ _ name; do
    [ "$name" = . ] || [ "$name" = .. ] || [ -z "$name" ] && continue
    if [ "${perm:0:1}" = d ]; then list "$1/$name"; else echo "$size $1/$name"; fi
  done
}
R=$1; L=$2; mkdir -p "$L"
list "$R" | grep -vE " $R/(eboot\.bin|sce_module/.*\.prx)$" > "$L/.filelist"
echo "LIST $(wc -l < "$L/.filelist") files, $(awk '{s+=$1} END{printf "%.1f GB", s/1e9}' "$L/.filelist")"
fetch() { size=$1; rp=$2; lp="$L/${rp#$R/}"; mkdir -p "$(dirname "$lp")"
  for t in 1 2 3 4 5 6; do curl -s -C - -o "$lp" "$H$rp"; rc=$?; [ $rc -eq 0 ] || [ $rc -eq 33 ] && break; sleep 5; done
  echo "DONE ${rp#$R/} $(stat -c %s "$lp")"; }
export -f fetch; export H R L
sort -n "$L/.filelist" | xargs -P3 -L1 bash -c 'fetch "$0" "$1"'
echo "ALLDONE $R"
