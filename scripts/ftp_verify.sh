#!/bin/bash
# wait for mirror.sh, then resume every non-SELF file until local size == remote size
H=ftp://192.168.1.10:2121; D=$(cd -- "$(dirname -- "$0")/../dump" && pwd)
LOG=${LOG:-$HOME/decomp/dump/.mirror.log}
while pgrep -f mirror.sh >/dev/null; do sleep 20; done
for pass in $(seq 1 30); do
  bad=0
  for pair in "$D/patch0:/mnt/sandbox/pfsmnt/CUSA03627-patch0" "$D:/mnt/sandbox/pfsmnt/CUSA03627-app0"; do
    L=${pair%%:*}; R=${pair#*:}
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
