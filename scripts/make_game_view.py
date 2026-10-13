#!/usr/bin/env python3
"""Builds the merged game folder a port runs from: the base game with the update on top.

usage: make_game_view.py <base_dir> <patch_dir> <out_dir>

Every file of the base appears in <out_dir> as a symlink; files of the update replace them
(eboot.bin, sce_sys/, sce_module/, the update's own archives such as *_Patch_*.psarc or *p.psarc),
merging folders recursively. Nothing is copied, so the view costs no disk space; re-run it after
replacing either dump. The update wins on conflicts, as on the console.
"""
import os
from pathlib import Path
import sys


# Things a dump folder may hold that are not part of the game (top level only).
NOT_GAME = {'patch0', 'patch-meta', 'system_lib', 'itemzflow-dumper.log', '.filelist'}


def link_tree(source: Path, target: Path, replace: bool, top: bool = True) -> int:
    count = 0
    target.mkdir(parents=True, exist_ok=True)
    for entry in sorted(source.iterdir()):
        if top and entry.name in NOT_GAME:
            continue
        dest = target / entry.name
        if entry.is_dir() and not entry.is_symlink():
            if dest.is_symlink():
                dest.unlink()  # a linked folder from the base becomes a real merged folder
            count += link_tree(entry, dest, replace, top=False)
            continue
        if dest.is_symlink() or dest.exists():
            if not replace:
                continue
            dest.unlink()
        os.symlink(entry.resolve(), dest)
        count += 1
    return count


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    base, patch, out = (Path(a).expanduser() for a in sys.argv[1:])
    if not (base / 'eboot.bin').is_file():
        sys.exit(f'{base}: no eboot.bin (base game dump expected)')
    if out.exists():
        for root, dirs, files in os.walk(out, topdown=False):  # only links and empty folders
            for name in files:
                path = Path(root) / name
                if not path.is_symlink():
                    sys.exit(f'{path}: not a link; refusing to clear {out}')
                path.unlink()
            for name in dirs:
                path = Path(root) / name
                path.unlink() if path.is_symlink() else path.rmdir()
    base_count = link_tree(base, out, replace=False)
    patch_count = link_tree(patch, out, replace=True) if patch.is_dir() else 0
    print(f'{out}: {base_count} base files, {patch_count} from the update')


if __name__ == '__main__':
    main()
