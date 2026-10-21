# SPDX-License-Identifier: GPL-2.0-or-later
"""The GPU shader cache of one game: where it is, how big it is, and clearing it.

The GPU backend (shadPS4 fork, video_core/cache_storage.cpp) keeps it in the port's own folder,
<gpu user>/cache/ (<gpu user> = TLG_GPU_USER_DIR, which the probe sets to its --user folder, the
saves folder, on every OS):
  <serial>-<recompiler key>/  translated shaders (*.spv, *.meta, *.key, profile.bin) and
                              pipeline-binaries/; other keys of the game are pruned at start;
  <serial>/, <serial>.zip     older unkeyed layout and the archived form.
Before, the folder was picked too early: ~/.local/share/shadPS4 on Linux (shadPS4's own folder,
shared with an installed shadPS4 emulator) and <saves>/user on Windows. The game moves its current
<serial>-<key> folder from there once; clearing also takes the port's leftovers there, but in the
shadPS4 folder only <serial>-<12 hex digits> folders (certainly the port's: the emulator names its
caches <serial>/ and <serial>.zip). Everything here is rebuilt on the next start; saves, settings,
logs and the console modules are never in it.
"""
import os
from pathlib import Path
import re
import shutil
from struct import unpack_from
import sys

SERIAL = re.compile(r'^[A-Z]{4}[0-9]{5}$')
# The port's keyed folder name: <serial>-<first 12 hex digits of TLG_SHADER_CACHE_KEY>.
KEYED = re.compile(r'^([A-Z]{4}[0-9]{5})-[0-9a-f]{12}$')


def title_id(game_dir):
    """TITLE_ID of <game>/sce_sys/param.sfo (the serial the GPU backend names its cache after)."""
    try:
        data = (Path(game_dir) / 'sce_sys' / 'param.sfo').read_bytes()
        magic, _, keys, values, count = unpack_from('<4sIIII', data, 0)
        if magic != b'\0PSF':
            return None
        for i in range(count):
            key, _, size, _, offset = unpack_from('<HHIII', data, 20 + i * 16)
            if data[keys + key:data.index(0, keys + key)] == b'TITLE_ID':
                return data[values + offset:values + offset + size].rstrip(b'\0').decode('ascii')
    except Exception:  # missing or damaged file: the known serials still apply
        return None
    return None


def cache_dirs(gpu_user_dir, env=None):
    """(folder, keyed_only) pairs: the port's cache, the old Windows one, the shared shadPS4 one."""
    env = os.environ if env is None else env
    gpu_user_dir = Path(gpu_user_dir)
    dirs = [(gpu_user_dir / 'cache', False), (gpu_user_dir / 'user' / 'cache', False)]
    if os.name != 'nt':
        data_home = env.get('XDG_DATA_HOME') or str(Path(env.get('HOME') or Path.home()) / '.local' / 'share')
        dirs.append((Path(data_home) / 'shadPS4' / 'cache', True))
    return dirs


def entries(serials, gpu_user_dir, env=None):
    """The cache entries of these serials (directories and archives), existing ones only."""
    serials = {s for s in serials if s and SERIAL.match(s)}
    found = []
    for cache, keyed_only in cache_dirs(gpu_user_dir, env):
        try:
            names = sorted(os.listdir(cache))
        except OSError:
            continue
        for name in names:
            if keyed_only:
                ours = bool((keyed := KEYED.match(name)) and keyed.group(1) in serials)
            else:
                stem = name[:-4] if name.endswith('.zip') else name
                ours = any(stem == s or stem.startswith(s + '-') for s in serials)
            path = cache / name
            if ours and path not in found:
                found.append(path)
    return found


def size_of(path):
    """Bytes in a file or a directory tree (symbolic links are not followed). os.scandir: on Windows
    the sizes come with the directory listing, while a stat per file opens each one (tens of thousands
    of cache files, each scanned by the antivirus, took long enough to freeze the launcher)."""
    path = Path(path)
    try:
        if path.is_symlink() or not path.is_dir():
            return 0 if path.is_symlink() else path.stat().st_size
    except OSError:
        return 0
    total, folders = 0, [str(path)]
    while folders:
        try:
            with os.scandir(folders.pop()) as listing:
                for entry in listing:
                    try:
                        if entry.is_dir(follow_symlinks=False):
                            folders.append(entry.path)
                        elif not entry.is_symlink():
                            total += entry.stat(follow_symlinks=False).st_size
                    except OSError:
                        pass
        except OSError:
            pass
    return total


def size(serials, gpu_user_dir, env=None):
    return sum(size_of(path) for path in entries(serials, gpu_user_dir, env))


def clear(serials, gpu_user_dir, env=None):
    """Deletes the cache of these serials; returns (bytes freed, [(path, error)] not deleted).
    A file in use (Windows, the game still open) stays and is reported; the rest goes."""
    freed, failures = 0, []
    for path in entries(serials, gpu_user_dir, env):
        before = size_of(path)
        errors = []
        try:
            if path.is_symlink() or not path.is_dir():
                path.unlink()
            elif sys.version_info >= (3, 12):
                shutil.rmtree(path, onexc=lambda _f, name, error: errors.append((Path(name), error)))
            else:
                shutil.rmtree(path, onerror=lambda _f, name, info: errors.append((Path(name), info[1])))
        except OSError as error:
            errors.append((path, error))
        freed += before - (size_of(path) if path.exists() or path.is_symlink() else 0)
        failures += errors
    return freed, failures


def human_size(count):
    for unit in ('B', 'KB', 'MB'):
        if count < 1024:
            return f'{count:.0f} {unit}' if unit == 'B' else f'{count:.1f} {unit}'
        count /= 1024
    return f'{count:.2f} GB'
