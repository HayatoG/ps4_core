#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Catalogue of the PS4 system functions the core implements (generates docs/LIBRARIES.md).

Sources, all read from the ps4_core tree:
  port/src/*.c (core only, per CORE_MANIFEST)  RuntimeExport tables ({"symbolName", fn}) and
                                               NID-keyed resolvers ("NID#library" -> fn)
  port/src/import_names.inc                    NID#library -> symbol name (runtime_lookup's index)
  port/gpu/shadps4/core/libraries/**.cpp       LIB_FUNCTION("NID", "library", ...) GPU-side HLE
Optional, from the ports (read only, never copied):
  <port>/port/src/runtime_<slug>*.c at --ref   per-game functions (not core; listed apart)
  <port>/port/out/link.json                    the system imports the game really asks for
                                               ("unresolved_imports" of link_modules.py), used for
                                               the TLG/GR2/ISS columns and the coverage table

"real" vs "stub" is a heuristic on the function body: a body that only returns a constant (or a
new id, or sets errno), with no branch and no call besides logging, is an offline stub.

  tools/list_exports.py                       print the Markdown to stdout
  tools/list_exports.py -o docs/LIBRARIES.md  write it
  tools/list_exports.py --no-ports            core only (no game columns)
"""
import argparse
import base64
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

CORE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CORE / 'tools'))
from core_sync import Manifest  # noqa: E402

# Port repo, short column name, per-game source globs (the files CORE_MANIFEST leaves out).
PORTS = (('tlg_pc', 'TLG'), ('gr2_pc', 'GR2'), ('infamous_pc', 'ISS'))
GAME_SOURCES = re.compile(r'port/src/runtime_(tlg|gr2|infamous)[^/]*\.c$')
OWNER = {'tlg': 'TLG', 'gr2': 'GR2', 'infamous': 'ISS'}

NID_SUFFIX = bytes.fromhex('518D64A635DED8C1E6B039B1C3E55230')
NID_RE = r'[A-Za-z0-9+\-]{11}'

# Library of a symbol that import_names.inc does not list, by prefix.
PREFIXES = (
    ('sceKernel', 'libkernel'), ('scePthread', 'libkernel'), ('pthread_', 'libkernel'),
    ('sceUserService', 'libSceUserService'), ('sceSystemService', 'libSceSystemService'),
    ('sceNetCtl', 'libSceNetCtl'), ('sceNet', 'libSceNet'), ('sceSsl', 'libSceSsl'),
    ('sceHttp', 'libSceHttp'), ('sceNpTrophy', 'libSceNpTrophy'), ('sceNpWebApi', 'libSceNpWebApi'),
    ('sceNpMatching2', 'libSceNpMatching2'), ('sceNpScore', 'libSceNpScore'),
    ('sceNpAuth', 'libSceNpManager'), ('sceNpLookup', 'libSceNpManager'), ('sceNp', 'libSceNpManager'),
    ('sceAudioOut', 'libSceAudioOut'), ('sceAudioIn', 'libSceAudioIn'), ('sceAjm', 'libSceAjm'),
    ('scePad', 'libScePad'), ('sceMouse', 'libSceMouse'), ('sceRtc', 'libSceRtc'),
    ('sceSaveDataDialog', 'libSceSaveDataDialog'), ('sceSaveData', 'libSceSaveData'),
    ('sceVideodec2', 'libSceVideodec2'), ('sceVideodec', 'libSceVideodec'),
    ('sceAppContent', 'libSceAppContent'), ('sceSysmodule', 'libSceSysmodule'),
    ('scePlayGo', 'libScePlayGo'), ('sceFios', 'libSceFios2'),
)

# Hand-written notes per library (kept here so the doc can be regenerated).
NOTES = {
    'libkernel': 'Threads, mutexes/conds/rwlocks/semaphores, event flags, TLS, direct and flexible '
                 'memory (host-owned low map), files through the /app0 and /savedata mounts, time. '
                 'libScePosix imports resolve here (runtime.c canonical_name). The GPU side adds '
                 'equeues, memory-pool and GPU-memory calls (shadPS4 kernel/).',
    'libc': 'The game ships its own libc.prx (linked by link_libc.py, native bindings); the core '
            'only provides the entry glue (_init_env, atexit, __cxa_*) and fast memcpy/memset.',
    'libSceGnmDriver': 'All in the GPU layer (shadPS4 gnmdriver): command buffers are translated to '
                       'Vulkan by the shadPS4 fork.',
    'libSceVideoOut': 'GPU layer (shadPS4 videoout + presenter): flip queue, buffers, vblank events; '
                      'presented through SDL3/Vulkan with the upscaler and post-processing.',
    'libScePad': 'SDL3 gamepads, keyboard and mouse; real gyro/accelerometer when the controller has '
                 'them, simulated motion otherwise; touchpad swipe; dead zone; TLG_KEY_MAP.',
    'libSceAudioOut': 'SDL3 audio streams per port; 3D audio for GR2 lives in runtime_gr2_audio3d.c.',
    'libSceAjm': 'ATRAC9 (LibAtrac9) and MP3 (runtime_mp3.c) decoding, batches and instances.',
    'libSceSaveData': 'Saves under port/user/savedata/<TITLE_ID>; mount, dir list, param, delete.',
    'libSceNpManager': 'Offline: the user is always signed out; requests are created and fail as '
                       'not available, so games keep going without PSN.',
    'libSceNpTrophy': 'Offline (game-side tables in the ports for now).',
    'libSceNet': 'Offline: pools/epolls/resolvers are created, every socket call fails with '
                 'ENETUNREACH.',
    'libSceHttp': 'Offline: templates/requests are created, sending fails as network down.',
    'libSceSsl': 'Init/term only.',
    'libSceNetCtl': 'Always disconnected.',
    'libSceUserService': 'One local user (id 1, name from the launcher).',
    'libSceSystemService': 'Params (language from the launcher, button assign...), status, events, '
                           'splash screen.',
    'libSceRtc': 'Clock, ticks and date conversions on the host clock.',
    'libSceAppContent': 'Boot params and an empty add-on list.',
    'libSceSysmodule': 'Load/unload report success; the modules are the core itself.',
    'libSceVideodec': 'H.264 through FFmpeg (libavcodec).',
    'libSceAvPlayer': 'GPU layer (shadPS4 avplayer on FFmpeg): intro movies.',
}

STUB_CALLS = re.compile(r'\b(new_id|LOG_\w+|printf|fprintf|puts|UNREACHABLE\w*|log_once|note)\s*\(')


def nid_of(symbol):
    digest = hashlib.sha1(symbol.encode() + NID_SUFFIX).digest()[:8][::-1]
    return base64.b64encode(digest).decode()[:11].replace('/', '-')


def canonical(scoped):
    nid, _, lib = scoped.partition('#')
    return f'{nid}#libkernel' if lib == 'libScePosix' else scoped


def strip_comments(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)


def function_bodies(text):
    """name -> body of every function definition in a C/C++ file (top level, brace matched)."""
    bodies = {}
    for m in re.finditer(r'\b(\w+)\s*\(([^;{}()]|\([^()]*\))*\)\s*(const\s*)?\{', text):
        name = m.group(1)
        if name in ('if', 'for', 'while', 'switch', 'return', 'sizeof') or name in bodies:
            continue
        depth, i = 0, m.end() - 1
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if not depth:
                    break
            i += 1
        bodies[name] = text[m.end():i]
    return bodies


def kind_of(body):
    if body is None:
        return '?'
    body = re.sub(r'\(void\)\s*\w+\s*;', '', body)
    body = re.sub(r'\b(LOG_\w+|printf|fprintf)\s*\((?:[^;]|"[^"]*")*?\)\s*;', '', body)
    if re.search(r'\b(if|for|while|switch|case)\b|\?', body):
        return 'real'
    statements = [s.strip() for s in body.split(';') if s.strip()]
    calls = re.findall(r'\b(\w+)\s*\(', body)
    if len(statements) <= 2 and all(STUB_CALLS.match(c + '(') or c in ('return',) for c in calls):
        return 'stub'
    return 'real'


class Entry:
    def __init__(self, lib, symbol, nid, function, file, kind, provider, scoped=None):
        self.lib, self.symbol, self.nid, self.function, self.scoped = lib, symbol, nid, function, scoped
        self.file, self.kind, self.provider = file, kind, provider
        self.games = set()
        self.nids = {nid}


def read_import_names(text):
    pairs = re.findall(r'\{"(' + NID_RE + r'#\w+)","([^"]*)"\}', text)
    by_scoped = {canonical(k): v for k, v in pairs}
    libs_of = defaultdict(set)
    for k, v in pairs:
        libs_of[v].add(canonical(k).split('#')[1])
        NIDS_OF[v].add(k.split('#')[0])
    return by_scoped, libs_of


# symbol -> NIDs listed for it in import_names.inc (placeholder names like
# sceDiscMap_7C980FFB0AA27E7A have no computable NID).
NIDS_OF = defaultdict(set)


def library_for(symbol, libs_of):
    libs = sorted(libs_of.get(symbol, ()))
    if libs:
        return libs[0] if len(libs) == 1 else next((x for x in libs if x != 'libkernel'), libs[0])
    for prefix, lib in PREFIXES:
        if symbol.startswith(prefix):
            return lib
    return 'libkernel' if not symbol.startswith('sce') else '(unknown)'


def scan_c(path_label, text, by_scoped, libs_of, provider):
    """Entries of one runtime C file: RuntimeExport tables and NID-keyed resolvers."""
    text = strip_comments(text)
    # String macros used as name prefixes: #define FUT "_ZN3sce7Toolkit..." then {FUT "...",fn}.
    for name, value in re.findall(r'#define\s+(\w+)\s+"([^"]*)"', text):
        text = re.sub(r'\b' + name + r'\s*"', '"' + value.replace('\\', '\\\\'), text)
    bodies = function_bodies(text)
    # Functions made by a function-like macro (DIALOG(profile,3) -> profile_init, ...;
    # TICK_ADD(rtc_add_hours, ...)): judged by the macro body.
    macros = {m.group(1): m.group(2) for m in re.finditer(r'#define\s+(\w+)\([^)]*\)((?:[^\n]*\\\n)*[^\n]*)', text)}
    for name, body in macros.items():
        for call in re.finditer(r'\b' + name + r'\(\s*(\w+)', text):
            first = call.group(1)
            for fn in re.findall(r'\b(\w*' + first + r'\w*)\b', text):
                if fn not in bodies and (fn == first or fn.startswith(first + '_') or fn.endswith('_' + first)):
                    bodies[fn] = body
    entries = []
    for table in re.finditer(r'RuntimeExport\s+\w+\[\]\s*=\s*\{(.*?)\};', text, re.S):
        for symbol, fn in re.findall(r'\{\s*"([^"#]+)"\s*,\s*&?(\w+)\s*\}', table.group(1)):
            nids = sorted(NIDS_OF.get(symbol) or {nid_of(symbol)})
            entry = Entry(library_for(symbol, libs_of), symbol, nids[0], fn, path_label,
                          kind_of(bodies.get(fn)), provider)
            entry.nids = set(nids)
            entries.append(entry)
    for scoped, fn in re.findall(r'"(' + NID_RE + r'#\w+)"\s*\)*\s*(?:\)\s*return\s*\(uintptr_t\)\s*&?|,\s*&?)(\w+)', text):
        scoped = canonical(scoped)
        symbol = by_scoped.get(scoped, scoped.split('#')[0])
        kind = 'data' if fn not in bodies and re.search(r'\b' + fn + r'\s*(=|;|\[)', text) else kind_of(bodies.get(fn))
        entries.append(Entry(scoped.split('#')[1], symbol, scoped.split('#')[0], fn, path_label, kind, provider, scoped))
    return entries


def scan_gpu(root, by_scoped):
    entries = []
    for path in sorted(root.rglob('*.cpp')):
        text = strip_comments(path.read_text(errors='replace'))
        if 'LIB_FUNCTION' not in text:
            continue
        bodies = function_bodies(text)
        label = str(path.relative_to(CORE))
        for nid, lib, fn in re.findall(r'LIB_FUNCTION\(\s*"(' + NID_RE + r')"\s*,\s*"(\w+)"\s*,[^,]*,[^,]*,\s*([\w:]+)\s*\)', text):
            scoped = canonical(f'{nid}#{lib}')
            short = fn.split('::')[-1]
            symbol = by_scoped.get(scoped, short)
            entries.append(Entry(scoped.split('#')[1], symbol, nid, short, label, kind_of(bodies.get(short)), 'gpu'))
    return entries


def git_text(repo, ref, path):
    out = subprocess.run(['git', '-C', str(repo), 'show', f'{ref}:{path}'], capture_output=True)
    return out.stdout.decode(errors='replace') if out.returncode == 0 else None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-o', '--output', type=Path, help='write the Markdown here (default stdout)')
    p.add_argument('--ref', default='origin/main', help='ref of the ports for their game files (default origin/main)')
    p.add_argument('--ports-dir', type=Path, default=CORE.parent, help='where tlg_pc/gr2_pc/infamous_pc live')
    p.add_argument('--no-ports', action='store_true', help='core only: no game columns, no coverage')
    args = p.parse_args()

    manifest = Manifest(CORE / 'CORE_MANIFEST')
    by_scoped, libs_of = read_import_names((CORE / 'port/src/import_names.inc').read_text())
    known_names = set(by_scoped.values())

    entries = []
    for path in sorted((CORE / 'port/src').glob('*.c')):
        rel = str(path.relative_to(CORE))
        if manifest.matches(rel):
            entries += scan_c(rel, path.read_text(errors='replace'), by_scoped, libs_of, 'core')
    entries += scan_gpu(CORE / 'port/gpu/shadps4/core/libraries', by_scoped)

    games = {}       # column -> set of canonical scoped imports
    game_files = {}  # column -> [Entry] of the port's own runtime_<slug>*.c
    link_dates = {}
    if not args.no_ports:
        for repo_name, col in PORTS:
            repo = (args.ports_dir / repo_name).expanduser()
            link = repo / 'port/out/link.json'
            if link.exists():
                data = json.loads(link.read_text())
                games[col] = {canonical(x) for x in data.get('unresolved_imports', [])}
                link_dates[col] = time.strftime('%Y-%m-%d', time.localtime(link.stat().st_mtime))
            listing = subprocess.run(['git', '-C', str(repo), 'ls-tree', '-r', '--name-only', args.ref, 'port/src/'],
                                     capture_output=True).stdout.decode().split()
            game_files[col] = []
            for rel in listing:
                if GAME_SOURCES.search(rel):
                    text = git_text(repo, args.ref, rel)
                    if text:
                        game_files[col] += scan_c(f'{repo_name}/{rel}', text, by_scoped, libs_of, col)

    # Which game imports each entry serves (by NID: a name table entry answers every NID whose
    # import_names.inc symbol is that name, i.e. its own NID).
    index = defaultdict(list)
    for e in entries:
        for n in e.nids:
            index[n].append(e)
    coverage = {}
    for col, imports in games.items():
        own = defaultdict(list)
        for e in game_files.get(col, []):
            for n in e.nids:
                own[n].append(e)
        counts = defaultdict(int)
        missing, borrowed = [], []
        for scoped in sorted(imports):
            nid = scoped.split('#')[0]
            # runtime_lookup: a name-table entry answers only when import_names.inc names this
            # scoped NID; a NID-keyed entry answers its exact "NID#library".
            core_hits = [e for e in index.get(nid, []) if e.provider == 'core' and
                         (e.scoped == scoped if e.scoped else scoped in by_scoped)]
            gpu_hits = [e for e in index.get(nid, []) if e.provider == 'gpu']
            for e in core_hits + gpu_hits:
                e.games.add(col)
            if core_hits:
                counts['core'] += 1
            elif [e for e in own.get(nid, []) if (e.scoped == scoped if e.scoped else scoped in by_scoped)]:
                counts['game'] += 1
                hit = own[nid][0]
                hit.games.add(col)
                owner = OWNER[GAME_SOURCES.search(hit.file).group(1)]
                if owner != col:
                    borrowed.append(f'{hit.symbol} ({Path(hit.file).name})')
            elif gpu_hits:
                counts['gpu'] += 1
            elif scoped in ('f7uOxY9mM1U#libkernel', 'ZT4ODD2Ts9o#libSceLibcInternal', 'djxxOmW6-aw#libkernel'):
                counts['core'] += 1
            else:
                counts['missing'] += 1
                missing.append(f'{by_scoped.get(scoped, "?")} ({scoped})')
        coverage[col] = (len(imports), counts, missing, borrowed)

    out = []
    w = out.append
    cols = [c for _, c in PORTS if c in games]
    w('<!-- Gerado por tools/list_exports.py: não edite à mão. / Generated by tools/list_exports.py: do not edit by hand. -->')
    w('# Bibliotecas do PS4 no núcleo / PS4 libraries in the core\n')
    w('**pt_BR.** Catálogo de cada biblioteca do sistema do PS4 que o `ps4_core` implementa: em que '
      'arquivo, quantas funções, quais são de verdade e quais são stubs offline (só devolvem um valor '
      'fixo para o jogo seguir sem PSN/rede), e quais jogos usam cada uma. Gerado a partir das tabelas '
      '`RuntimeExport` e dos resolvers por NID (`port/src/*.c`), do `import_names.inc` e dos '
      '`LIB_FUNCTION` da GPU (`port/gpu/shadps4/core/libraries`). As colunas TLG/GR2/ISS vêm do '
      '`port/out/link.json` de cada port (os imports de sistema que o jogo pede de fato). Para '
      'regenerar: `tools/list_exports.py -o docs/LIBRARIES.md`.\n')
    w('**English.** Catalogue of every PS4 system library the core implements: file(s), function '
      'count, real implementations vs offline stubs (return a fixed value so the game proceeds without '
      'PSN/network), and which games exercise each one. Built from the `RuntimeExport` tables and '
      'NID-keyed resolvers, `import_names.inc` and the GPU `LIB_FUNCTION` registrations. Game columns '
      'come from each port\'s `port/out/link.json`. "real"/"stub" is a heuristic on the function body. '
      'Regenerate with `tools/list_exports.py -o docs/LIBRARIES.md`.\n')
    if link_dates:
        w('Game import lists / listas de imports: ' + ', '.join(f'{c} `link.json` {d}' for c, d in link_dates.items())
          + f'; game files at `{args.ref}`.\n')

    # Coverage
    if coverage:
        w('## Cobertura por jogo / Coverage per game\n')
        w('Who answers each system import of the game, in `runtime_resolve` order: core runtime, then the '
          'game\'s own `runtime_<slug>*.c`, then the GPU layer. "missing" resolves to nothing (the loader '
          'binds a trap/zero stub).\n')
        w('| Game | System imports | Core runtime | Game files | of which another game\'s file | GPU layer | Missing |')
        w('|---|---|---|---|---|---|---|')
        for col in cols:
            total, c, _, borrowed = coverage[col]
            w(f'| {col} | {total} | {c["core"]} | {c["game"]} | {len(borrowed)} | {c["gpu"]} | {c["missing"]} |')
        w('')
        w('"Another game\'s file": the import is answered by a `runtime_<other slug>*.c` the port carries '
          '(e.g. inFAMOUS using GR2\'s files). Those functions are shared in practice and are the first '
          'candidates to move into the core.\n')
        for col in cols:
            borrowed = coverage[col][3]
            if borrowed:
                w(f'<details><summary>{col}: {len(borrowed)} from another game\'s file</summary>\n')
                w(', '.join(f'`{b}`' for b in borrowed) + '\n')
                w('</details>\n')
        for col in cols:
            missing = coverage[col][2]
            if missing:
                w(f'<details><summary>{col}: {len(missing)} missing</summary>\n')
                w(', '.join(f'`{m}`' for m in missing) + '\n')
                w('</details>\n')

    # Summary per library
    libs = defaultdict(list)
    for e in entries:
        libs[e.lib].append(e)
    w('## Resumo / Summary\n')
    head = '| Library | Files | Functions | Real | Stub | ' + ' | '.join(cols) + (' | ' if cols else '') + 'Notes |'
    w(head)
    w('|' + '---|' * (head.count('|') - 1))
    for lib in sorted(libs, key=lambda x: (-len(libs[x]), x)):
        es = libs[lib]
        files = sorted({Path(e.file).name for e in es})
        real = sum(e.kind == 'real' for e in es)
        stub = sum(e.kind == 'stub' for e in es)
        used = [str(sum(col in e.games for e in es) or '·') for col in cols]
        note = NOTES.get(lib, '')
        w(f'| `{lib}` | {", ".join(f"`{f}`" for f in files)} | {len(es)} | {real} | {stub} | '
          + ''.join(u + ' | ' for u in used) + f'{note} |')
    w('')

    unreachable = sorted({e.symbol for e in entries if e.provider == 'core' and '#' not in e.file
                          and e.symbol not in known_names and not re.fullmatch(NID_RE, e.symbol)})
    if unreachable:
        w('### Nomes fora do `import_names.inc` / Names missing from `import_names.inc`\n')
        w('`runtime_lookup` resolves a name only through `import_names.inc` (NID → name), so these table '
          'entries are unreachable until their NID line is added (generate with `scripts/sce_imports.py`; '
          'the NID is `tools/list_exports.py`\'s `nid_of(name)`):\n')
        w(', '.join(f'`{s}` (`{nid_of(s)}`)' for s in unreachable) + '\n')

    # Details
    w('## Funções por biblioteca / Functions per library\n')
    w('Kind: **real** = implemented; *stub* = offline/fixed result; data = a variable; ? = body not found. '
      'Games: which game imports it.\n')
    for lib in sorted(libs):
        es = sorted(libs[lib], key=lambda e: (e.provider != 'core', e.symbol))
        w(f'<details><summary><code>{lib}</code> — {len(es)}</summary>\n')
        w('| Function | NID | Kind | Implemented in | Games |')
        w('|---|---|---|---|---|')
        for e in es:
            kind = {'real': '**real**', 'stub': '*stub*'}.get(e.kind, e.kind)
            w(f'| `{e.symbol}` | `{e.nid}` | {kind} | `{e.file.replace("port/gpu/shadps4/core/libraries/", "gpu …/")}`'
              f' (`{e.function}`) | {" ".join(c for c in cols if c in e.games)} |')
        w('\n</details>\n')

    # Per-game files
    if any(game_files.values()):
        w('## Fora do núcleo: arquivos de cada jogo / Outside the core: per-game files\n')
        w('Functions the ports still implement in their own `runtime_<slug>*.c` (candidates to move into '
          'the core when a second game needs them). Each file is counted once, from the port that owns it; "Copies in" lists the ports that also carry it (today runtime.c links every resolver, see `port/src/runtime_game.h`).\n')
        w('| Owner | File | Library | Functions | Real | Stub | Copies in |')
        w('|---|---|---|---|---|---|---|')
        copies = defaultdict(set)
        groups = defaultdict(list)
        for repo_name, col in PORTS:
            for e in game_files.get(col, []):
                name = Path(e.file).name
                copies[name].add(col)
                if OWNER[GAME_SOURCES.search(e.file).group(1)] == col:
                    groups[(col, name, e.lib)].append(e)
        for (col, name, lib), es in sorted(groups.items(), key=lambda kv: ([c for _, c in PORTS].index(kv[0][0]), kv[0][1:])):
            others = ' '.join(c for _, c in PORTS if c in copies[name] and c != col)
            w(f'| {col} | `{name}` | `{lib}` | {len(es)} | {sum(e.kind == "real" for e in es)} '
              f'| {sum(e.kind == "stub" for e in es)} | {others} |')
        w('')

    text = '\n'.join(out) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(f'wrote {args.output}: {len(entries)} core/GPU entries in {len(libs)} libraries')
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
