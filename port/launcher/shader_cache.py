# SPDX-License-Identifier: GPL-2.0-or-later
"""The GPU shader cache of one game: where it is, how big it is, and clearing it.

The GPU backend (shadPS4 fork, video_core/cache_storage.cpp) keeps under <gpu user>/cache/:
  <serial>-<recompiler key>/  translated shaders (*.spv, *.meta, *.key, profile.bin); a new
                              recompiler gets a new key and the old directories are removed;
  <serial>/                   the same without a key, and pipeline-binaries/ (VK_KHR_pipeline_binary);
  <serial>.zip                the archived form of the same cache.
<gpu user> is TLG_GPU_USER_DIR, else user/ in the game's working directory (the saves folder:
port/user/user on Windows, where it is always created), else ~/.local/share/shadPS4 on Linux, which
every port shares: only the entries of this game's serial are touched. Everything here is rebuilt on
the next start; saves, settings, logs and the console modules are never in it.
"""
import os
from pathlib import Path
import re
import shutil
from struct import unpack_from
import sys

SERIAL = re.compile(r'^[A-Z]{4}[0-9]{5}$')


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


def gpu_user_dirs(user_dir, env=None):
    """Every folder the GPU backend may have used as its user folder for this port."""
    env = os.environ if env is None else env
    user_dir = Path(user_dir)
    candidates = [Path(env['TLG_GPU_USER_DIR'])] if env.get('TLG_GPU_USER_DIR') else []
    candidates += [user_dir, user_dir / 'user']
    if os.name != 'nt':
        data_home = env.get('XDG_DATA_HOME') or str(Path(env.get('HOME') or Path.home()) / '.local' / 'share')
        candidates.append(Path(data_home) / 'shadPS4')
    unique = []
    for path in candidates:
        if path not in unique:
            unique.append(path)
    return unique


def entries(serials, user_dir, env=None):
    """The cache entries of these serials (directories and archives), existing ones only."""
    serials = {s for s in serials if s and SERIAL.match(s)}
    found = []
    for base in gpu_user_dirs(user_dir, env):
        cache = base / 'cache'
        try:
            names = sorted(os.listdir(cache))
        except OSError:
            continue
        for name in names:
            stem = name[:-4] if name.endswith('.zip') else name
            if any(stem == s or stem.startswith(s + '-') for s in serials):
                path = cache / name
                if path not in found:
                    found.append(path)
    return found


def size_of(path):
    """Bytes in a file or a directory tree (symbolic links are not followed)."""
    path = Path(path)
    try:
        if path.is_symlink() or not path.is_dir():
            return 0 if path.is_symlink() else path.stat().st_size
    except OSError:
        return 0
    total = 0
    for folder, _, files in os.walk(path):
        for name in files:
            try:
                total += os.lstat(os.path.join(folder, name)).st_size
            except OSError:
                pass
    return total


def size(serials, user_dir, env=None):
    return sum(size_of(path) for path in entries(serials, user_dir, env))


def clear(serials, user_dir, env=None):
    """Deletes the cache of these serials; returns (bytes freed, [(path, error)] not deleted).
    A file in use (Windows, the game still open) stays and is reported; the rest goes."""
    freed, failures = 0, []
    for path in entries(serials, user_dir, env):
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
