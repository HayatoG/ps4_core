#!/usr/bin/env bash
# Vigia uma sessão do jogo: avisa quando abre/fecha/cai e, se o jogo parar de apresentar frames
# por $STALL segundos (sem "Frame stats" no log), tira um dump das threads com kill -USR1.
# Uma linha por evento (feito para o Monitor do Claude Code). Resumo de FPS a cada $SUMMARY s.
# Linux; precisa de ISS_FRAME_STATS=1 (o launcher já liga). LOGS: pasta dos run-*.log (padrão port/out/logs).
STALL=${STALL:-20}; SUMMARY=${SUMMARY:-120}
logs=${LOGS:-$(cd -- "$(dirname -- "$0")/.." && pwd)/port/out/logs}
pid=""; log=""; dumped=0; last_sum=$(date +%s)
while true; do
  p=$(pgrep -f "tlg-probe .*boot-linked.bin" | head -1)
  if [[ -n $p && $p != "$pid" ]]; then
    pid=$p; dumped=0; sleep 2; log=$(ls -1t "$logs"/run-*.log | head -1)
    echo "Sessão iniciada (pid $pid, log $(basename "$log"))"
  fi
  if [[ -n $pid && -z $p ]]; then
    last=$(grep -E "STOP|fault \(signal|Assertion" "$log" | tail -1)
    echo "Sessão encerrada (pid $pid). ${last:-Fechado normalmente.}"
    pid=""
  fi
  if [[ -n $pid ]]; then
    lastfs=$(grep -n "^Frame stats:" "$log" | tail -1)
    age=$(( $(date +%s) - $(stat -c %Y "$log") ))
    fs_line=${lastfs%%:*}; total=$(wc -l < "$log")
    # sem "Frame stats" nas últimas linhas e log parado há STALL s => travado
    if [[ -n $lastfs ]] && (( dumped == 0 )); then
      since=$(tail -n +"$fs_line" "$log" | grep -c "^Frame stats:")
      lastfs_age=$(( $(date +%s) - $(stat -c %Y "$log") ))
      newer=$(awk -v n="$fs_line" 'NR>n' "$log" | grep -c "^Frame stats:")
    fi
    if [[ -n $lastfs ]] && (( dumped == 0 )); then
      ts_file=/tmp/.tlg_fs_$pid
      cur=$(grep -c "^Frame stats:" "$log")
      prev=$(cat "$ts_file" 2>/dev/null || echo "0 $(date +%s)")
      read pc pt <<<"$prev"
      if (( cur != pc )); then echo "$cur $(date +%s)" > "$ts_file"
      elif (( $(date +%s) - pt >= STALL )); then
        kill -USR1 "$pid" 2>/dev/null; dumped=1
        echo "TRAVOU: ${STALL}s sem frames (último: $(grep '^Frame stats:' "$log" | tail -1 | cut -c1-60)). Dump das threads gravado no log."
      fi
    fi
    if (( $(date +%s) - last_sum >= SUMMARY )); then
      last_sum=$(date +%s)
      avg=$(grep "^Frame stats:" "$log" | tail -24 | awk '{s+=$3; n++} END{if(n) printf "%.1f", s/n}')
      echo "Resumo: ~${avg:-?} FPS médios nos últimos 2 min; $(grep -c 'memory pressure' "$log") avisos de VRAM; $(grep -c '^Stall:' "$log") engasgos >40 ms na sessão"
    fi
  fi
  sleep 3
done
