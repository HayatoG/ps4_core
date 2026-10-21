#!/bin/bash
# Itemzflow USB dump progress, read via PS4 FTP LIST (64-bit sizes)
# PS4_FTP, TITLE_ID: as in ftp_mirror.sh; USB_DIR: the dump folder on the PS4 (default /mnt/usb0/<title>).
# TOTAL_BYTES: the expected size (default: the app0 listing in dump/.filelist, else ~27 GB, the base game).
T=${TITLE_ID:-CUSA00223}
H=${PS4_FTP:-ftp://192.168.1.10:2121}${USB_DIR:-/mnt/usb0/$T}
list=$(dirname -- "$0")/../dump/.filelist
TOTAL=${TOTAL_BYTES:-$( [ -f "$list" ] && awk '{s+=$1} END{print s}' "$list" || echo 27000000000 )}
last=-1; prevb=-1; prevt=$(date +%s); stall=0
while true; do
  B=$(curl -s --max-time 20 "$H/" | awk '$1 ~ /^-/ {s+=$5} END{print s+0}')
  [ -z "$B" ] || [ "$B" = 0 ] && { sleep 30; continue; }
  now=$(date +%s); [ $prevb -lt 0 ] && { prevb=$B; prevt=$now; }
  pct=$((B*100/TOTAL)); pct5=$((pct/5*5))
  if [ $pct5 -ne $last ]; then
    dt=$((now-prevt)); rate=$(( dt>0 ? (B-prevb)/dt/1000000 : 0 ))
    eta=$( [ $rate -gt 0 ] && echo "$(( (TOTAL-B)/1000000/rate/60 )) min" || echo "?")
    printf "Dump USB: %d%% (%.1f / %.1f GB) ~%d MB/s, ETA %s\n" $pct $(echo "$B/1000000000" | bc -l) $(echo "$TOTAL/1000000000" | bc -l) $rate "$eta"
    last=$pct5; prevb=$B; prevt=$now; stall=0
  else stall=$((stall+1)); fi
  [ $pct -ge 100 ] && { echo "Dump USB: tamanho total atingido — confira no Itemzflow se terminou"; exit 0; }
  [ $stall -eq 20 ] && echo "Dump USB: sem progresso há ~10 min (parado em $pct%) — terminou ou travou?"
  sleep 30
done
