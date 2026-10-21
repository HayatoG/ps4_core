#!/bin/bash
# wait for ftp_mirror.sh, then resume every non-SELF file until local size == remote size, for the
# app0 mirror in DUMP_DIR (default dump/) and the update in DUMP_DIR/patch0 (if it has a .filelist).
# PS4_FTP, TITLE_ID: as in ftp_mirror.sh. LOG: default DUMP_DIR/.mirror.log.
H=${PS4_FTP:-ftp://192.168.1.10:2121}; T=${TITLE_ID:-CUSA00223}
D=${DUMP_DIR:-$(cd -- "$(dirname -- "$0")/.." && pwd)/dump}
LOG=${LOG:-$D/.mirror.log}
while pgrep -f ftp_mirror.sh >/dev/null; do sleep 20; done
for pass in $(seq 1 30); do
  bad=0
  for pair in "$D/patch0:/mnt/sandbox/pfsmnt/$T-patch0" "$D:/mnt/sandbox/pfsmnt/$T-app0"; do
    L=${pair%%:*}; R=${pair#*:}
    [ -f "$L/.filelist" ] || continue
    while read sz p; do
      case "$p" in *.sprx|*.prx|*/eboot.bin) continue;; esac
      lp="$L/${p#$R/}"; a=$(stat -c %s "$lp" 2>/dev/null || echo 0)
      if [ "$a" -lt "$sz" ]; then bad=1; echo "RESUME ${p##*/} $a/$sz" >> $LOG; curl -s -C - -o "$lp" "$H$p"
      elif [ "$a" -gt "$sz" ]; then echo "OVERSIZE ${p##*/} $a/$sz" >> $LOG; fi
    done < "$L/.filelist"
  done
  [ $bad -eq 0 ] && { echo "VERIFIED all sizes match (pass $pass)" >> $LOG; exit 0; }
  sleep 5
done
echo "VERIFY FAILED after 30 passes" >> $LOG; exit 1
