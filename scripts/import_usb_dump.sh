#!/usr/bin/env bash
# Importa do pendrive (rótulo TLGDUMP, dump do Itemzflow) os psarc que o FTP do GoldHEN truncou (>4 GiB),
# verificando contra o que já existe em dump/:
#   1. os bytes já baixados (prefixo truncado) precisam ser idênticos no pendrive;
#   2. o tamanho final precisa bater com o LIST do PS4 (dump/.filelist);
#   3. os demais psarc do pendrive precisam ser idênticos aos locais (cross-check do dump USB).
set -euo pipefail
dump=${DUMP_DIR:-$HOME/decomp/dump}
dev=/dev/disk/by-label/TLGDUMP
[[ -e $dev ]] || { echo "Pendrive TLGDUMP não encontrado" >&2; exit 1; }
mnt=$(findmnt -n -o TARGET "$dev" || true)
if [[ -z $mnt ]]; then udisksctl mount -b "$dev" >/dev/null; mnt=$(findmnt -n -o TARGET "$dev"); fi
src=$(find "$mnt" -maxdepth 3 -type f -name trico_boot.psarc -printf '%h\n' | head -1)
[[ -n $src ]] || { echo "Nenhum trico_boot.psarc em $mnt" >&2; exit 1; }
echo "Origem: $src"
expected() { awk -v f="/$1" '$2 ~ f"$" {print $1}' "$dump/.filelist"; }
ok=1
for f in trico_001.psarc trico_002.psarc trico_003.psarc; do
    want=$(expected "$f"); have=$(stat -c %s "$src/$f")
    [[ $have == "$want" ]] || { echo "ERRO $f: pendrive tem $have bytes, PS4 lista $want"; ok=0; continue; }
    part=$(stat -c %s "$dump/$f")
    if ! cmp -s -n "$part" "$src/$f" "$dump/$f"; then echo "ERRO $f: primeiros $part bytes diferem do download FTP"; ok=0; continue; fi
    echo "$f: prefixo de $part bytes idêntico; copiando $((want-part)) bytes restantes..."
    tail -c +"$((part+1))" "$src/$f" >> "$dump/$f"
    [[ $(stat -c %s "$dump/$f") == "$want" ]] && echo "  OK $f completo ($want bytes)" || { echo "  ERRO tamanho final de $f"; ok=0; }
done
for f in trico_boot.psarc trico_enu.psarc trico_esl.psarc trico_frc.psarc trico_ptb.psarc sce_discmap.plt; do
    [[ -f $src/$f ]] || { echo "aviso: $f ausente no pendrive"; continue; }
    if cmp -s "$src/$f" "$dump/$f"; then echo "idêntico: $f"; else echo "ERRO: $f difere entre pendrive e FTP"; ok=0; fi
done
(( ok )) && echo "IMPORTAÇÃO OK" || { echo "IMPORTAÇÃO COM ERROS"; exit 1; }
