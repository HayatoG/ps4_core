#!/usr/bin/env python3
"""Builds the merged game folder a port runs from: the base game with the update on top.

usage: make_game_view.py <base_dir> <patch_dir> <out_dir> [update_param_sfo]

Every file of the base appears in <out_dir> as a symlink; files of the update replace them
(eboot.bin, sce_sys/, sce_module/, the update's own archives such as *_Patch_*.psarc or *p.psarc),
merging folders recursively. Nothing is copied, so the view costs no disk space; re-run it after
replacing either dump. The update wins on conflicts, as on the console.

The update's sce_sys usually lacks its param.sfo (the console keeps the 1.07 one in
/system_data/priv/appmeta/CUSA00223/param.sfo). Give that copy as [update_param_sfo] or put it in a
patch-meta/ folder inside the update, next to it or inside the base: the view links the param.sfo
with the highest APP_VER, so the base 1.00 one never hides it.
"""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'port' / 'scripts'))
from prepare import sfo  # noqa: E402


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


def app_version(path: Path) -> tuple:
    """APP_VER of a param.sfo as numbers ((1, 7) for "01.07"); () when it cannot be read."""
    try:
        return tuple(int(part) for part in str(sfo(path.read_bytes()).get('APP_VER', '')).split('.'))
    except (OSError, ValueError, IndexError, UnicodeDecodeError):
        return ()


def link_update_param(base: Path, patch: Path, out: Path, given: Path = None):
    """Links out/sce_sys/param.sfo to the newest one among the merged view's, the given one and
    patch-meta/param.sfo (in the update, next to it or in the base). Returns the source, or None."""
    target = out / 'sce_sys' / 'param.sfo'
    candidates = [given] if given else []
    candidates += [folder / 'patch-meta' / 'param.sfo' for folder in (patch, patch.parent, base)]
    best, best_version = None, app_version(target)
    for path in candidates:
        version = app_version(path)
        if version > best_version:
            best, best_version = path, version
    if best is None:
        return None
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink() or target.exists():
        target.unlink()
    os.symlink(best.resolve(), target)
    return best


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    base, patch, out = (Path(a).expanduser() for a in sys.argv[1:4])
    given = Path(sys.argv[4]).expanduser() if len(sys.argv) == 5 else None
    if given and not app_version(given):
        sys.exit(f'{given}: not a readable param.sfo')
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
    param = link_update_param(base, patch, out, given)
    version = '.'.join(f'{n:02d}' for n in app_version(out / 'sce_sys' / 'param.sfo')) or 'unreadable'
    print(f'sce_sys/param.sfo: APP_VER {version}' + (f' (from {param})' if param else ''))


if __name__ == '__main__':
    main()
