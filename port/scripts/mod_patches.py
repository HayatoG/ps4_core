#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generic shadPS4/GoldHEN cheat-XML loader, shared by every port's own <game>_patches.py.

Reads third-party patch XML files (the "PATCHES" format shadPS4/GoldHEN cheat tools use) from
a user-writable mods directory, keeps only the ones that match this game's title ID and app
version, and compiles their literal byte writes into (offset, bytes, description) triples the
caller merges into its own G3PATCH2 patch file (the loader, probe.c apply_patches, already
applies that format). A patch whose lines fall outside the eboot's segments is skipped on its
own, never aborting the others or the port's own built-in patches.

Address convention: shadPS4/GoldHEN XML "Address" values assume the eboot mapped at 0x400000,
not any one port's own Ghidra base - that is the convention the cheat files on the internet
are already written for, independent of which PS4 game or port reads them.

Disabled by default: a patch only has any effect once the player (or the file's own
isEnabled="true") turns it on in mods.json, same as shadPS4/GoldHEN. With nothing enabled this
module writes nothing, so a port that never looks at it is byte-for-byte unaffected.
"""
import json
import struct
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

EBOOT_BASE = 0x400000


def eboot_segments(elf):
    """[(vaddr, vaddr+memsz)] of the ELF's PT_LOAD segments, for validating patch addresses."""
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    segments = []
    for i in range(phnum):
        kind, _, _, vaddr, _, _, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize)
        if kind == 1:
            segments.append((vaddr, vaddr + memsz))
    return segments


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.03"), or None when it cannot be read."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('APP_VER')
    except (OSError, ValueError, ImportError):
        return None


def encode(line):
    kind, value = line.get('Type'), line.get('Value')
    if kind == 'bytes':
        return bytes.fromhex(value.replace(' ', ''))
    if kind in ('bytes16', 'bytes32', 'bytes64'):
        return int(value, 0).to_bytes(int(kind[5:]) // 8, 'little')
    if kind == 'float32':
        return struct.pack('<f', float(value))
    if kind == 'float64':
        return struct.pack('<d', float(value))
    if kind == 'utf8':
        return value.encode() + b'\0'
    if kind == 'utf16':
        return value.encode('utf-16-le') + b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def external_patches(directory, app_version, ids):
    """[(key, path, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection key, mods.json). ids: this
    game's title IDs; a file that scopes itself with <ID> entries is kept only when one of
    them is ours, a file with none at all is assumed to be written for this game already."""
    found = []
    directory = Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Mods: {path.name}: {error}', file=sys.stderr)
            continue
        file_ids = {e.text.strip() for e in root.iter('ID') if e.text}
        if file_ids and not file_ids & ids:
            continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer') == app_version and meta.get('AppElf', 'eboot.bin') == 'eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}', path, meta))
    return found


def external_selection(found, config):
    """Selected patches: mods.json {"enabled": [...], "disabled": [...]} overrides each file's
    own isEnabled. Nothing the player has not opted into (file default or mods.json) is ever
    returned, so an untouched mods folder changes no bytes."""
    settings = {}
    if config and Path(config).is_file():
        try:
            settings = json.loads(Path(config).read_text())
        except (OSError, ValueError) as error:
            print(f'Mods: cannot read {config}: {error}', file=sys.stderr)
    enabled, disabled = set(settings.get('enabled', [])), set(settings.get('disabled', []))
    return [(key, path, meta) for key, path, meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled', 'false').lower() == 'true')]


def compile_external(selected, segments):
    """[(offset, bytes, description)] writes of the selected patches. A patch with any line
    outside the eboot's segments is skipped whole, so a mismatched or malformed cheat just
    does not apply instead of writing somewhere wrong."""
    writes = []
    for key, _, meta in selected:
        try:
            ours = []
            for line in meta.iter('Line'):
                offset = int(line.get('Address') or '', 0) - EBOOT_BASE
                data = encode(line)
                if not any(start <= offset and offset + len(data) <= end for start, end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset, data, f'{key} ({line.get("Address")})'))
        except ValueError as error:
            print(f'Mods: skipped {key}: {error}', file=sys.stderr)
            continue
        writes += ours
        print(f'Mods: {key} ({len(ours)} writes)')
    return writes


def catalogue(directory, app_version, ids):
    """[(key, name, note, enabled_by_file)] of every patch found for this version, whether or
    not it is currently selected - for a launcher's Mods tab to list and let the player toggle."""
    return [(key, meta.get('Name'), meta.get('Note', ''), meta.get('isEnabled', 'false').lower() == 'true')
            for key, _, meta in external_patches(directory, app_version, ids)]
