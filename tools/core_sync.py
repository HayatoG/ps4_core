#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare the shared core (ps4_core) with the copy embedded in a port repository.

While the ports (tlg_pc, gr2_pc, infamous_pc) still carry their own copy of the core, this
tool keeps ps4_core up to date in parallel. The core files are the ones CORE_MANIFEST lists;
the relative paths are identical here and in every port.

  core_sync.py ~/decomp/tlg_pc                 list core files that differ (port working tree)
  core_sync.py --ref main ~/decomp/gr2_pc      same, against a branch/commit of the port
  core_sync.py --diff ~/decomp/gr2_pc          also show a unified diff (core -> port)
  core_sync.py --pull ~/decomp/infamous_pc     copy the port's NEWER core files into ps4_core
  core_sync.py --summary --ref main A B C      one line per port
  core_sync.py --status-all [--fetch]          sync table of tlg_pc, gr2_pc and infamous_pc (origin/main)

Status of each differing file:
  port-newer   the ps4_core version is in the port's history: the port moved past it (--pull takes it)
  core-newer   the port version is in the history of ps4_core or of its lineage (CORE_SOURCE): the port is behind
  diverged     neither contains the other (both changed); --pull --force takes the port's copy
  port-only    core file (by the manifest) that exists only in the port (--pull takes it)
  core-only    file the port does not have
  submodule    submodule at a different commit (never copied; the command to update is printed)

--pull only writes files into the ps4_core working tree; review with `git diff` and commit by hand.
"""
import argparse
import difflib
import fnmatch
import hashlib
import os
import subprocess
import sys
from pathlib import Path

CORE = Path(__file__).resolve().parent.parent


def git(repo, *args, check=True):
    result = subprocess.run(['git', '-C', str(repo), *args], capture_output=True)
    if check and result.returncode:
        sys.exit(f'git {" ".join(args)} failed in {repo}: {result.stderr.decode(errors="replace").strip()}')
    return result.stdout


def blob_hash(data):
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


class Manifest:
    """Include globs, '!' excludes and '@' submodules. fnmatch: '*' also crosses '/'."""

    def __init__(self, path):
        self.include, self.exclude, self.submodules = [], [], []
        for line in path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith('!'):
                self.exclude.append(line[1:])
            elif line.startswith('@'):
                self.submodules.append(line[1:])
            else:
                self.include.append(line)

    def matches(self, path):
        if path in self.submodules:
            return False
        return (any(fnmatch.fnmatchcase(path, g) for g in self.include)
                and not any(fnmatch.fnmatchcase(path, g) for g in self.exclude))


class Tree:
    """Tracked files (blob hash + content) of a repo, either a commit or the working tree."""

    def __init__(self, repo, ref=None):
        self.repo, self.ref = Path(repo), ref
        self.blobs, self.links, self.executable = {}, {}, set()
        if ref:
            out = git(self.repo, 'ls-tree', '-r', '-z', '--full-tree', ref)
            for entry in filter(None, out.split(b'\0')):
                meta, path = entry.split(b'\t', 1)
                mode, kind, sha = meta.split()
                target = self.links if kind == b'commit' else self.blobs
                target[path.decode()] = sha.decode()
                if mode == b'100755':
                    self.executable.add(path.decode())
        else:
            out = git(self.repo, 'ls-files', '-s', '-z')
            for entry in filter(None, out.split(b'\0')):
                meta, path = entry.split(b'\t', 1)
                mode, sha, _stage = meta.split()
                path = path.decode()
                if mode == b'160000':
                    self.links[path] = sha.decode()
                elif (self.repo / path).is_file():
                    self.blobs[path] = None  # hashed lazily from disk (may be modified)
                    if os.access(self.repo / path, os.X_OK):
                        self.executable.add(path)
            # Untracked but not ignored: new files count too (e.g. a fresh --pull into ps4_core).
            out = git(self.repo, 'ls-files', '-o', '--exclude-standard', '-z')
            for path in filter(None, out.decode().split('\0')):
                self.blobs[path] = None

    def read(self, path):
        if self.ref:
            return git(self.repo, 'cat-file', 'blob', self.blobs[path])
        return (self.repo / path).read_bytes()

    def sha(self, path):
        if self.blobs[path] is None:
            self.blobs[path] = blob_hash(self.read(path))
        return self.blobs[path]

    def history(self, path):
        """Every blob this path ever had in the history of ref (or HEAD), merge results included (-m)."""
        out = git(self.repo, 'log', '-m', '--format=', '--raw', '--no-abbrev', '--no-renames',
                  self.ref or 'HEAD', '--', path, check=False)
        seen = set()
        for line in out.decode(errors='replace').splitlines():
            if line.startswith(':'):
                fields = line[1:].split('\t', 1)[0].split()
                seen.update(fields[2:4])
        return seen


def lineage():
    """Repos whose history ps4_core continues (CORE_SOURCE lines: '<repo> <ref> [commit]')."""
    repos = []
    source = CORE / 'CORE_SOURCE'
    if source.exists():
        for line in source.read_text().splitlines():
            parts = line.split('#', 1)[0].split()
            if len(parts) >= 2 and Path(os.path.expanduser(parts[0])).is_dir():
                # The pinned commit, when given: a moving ref (origin/main) would also contain
                # the port's later changes and report them as "core-newer".
                repos.append(History(Path(os.path.expanduser(parts[0])), parts[2] if len(parts) > 2 else parts[1]))
    return repos


class History:
    """Only the history lookup of a Tree, for lineage repos."""

    def __init__(self, repo, ref):
        self.repo, self.ref = repo, ref

    history = Tree.history


def compare(port, ref, manifest, core, ancestors):
    """Yield (status, path) for every core file that differs."""
    paths = sorted({p for p in port.blobs if manifest.matches(p)} |
                   {p for p in core.blobs if manifest.matches(p)})
    for path in paths:
        if path not in core.blobs:
            yield 'port-only', path
            continue
        if path not in port.blobs:
            yield 'core-only', path
            continue
        core_sha, port_sha = core.sha(path), port.sha(path)
        if core_sha == port_sha:
            continue
        if core_sha in port.history(path):
            yield 'port-newer', path
        elif port_sha in core.history(path) or any(port_sha in a.history(path) for a in ancestors):
            yield 'core-newer', path
        else:
            yield 'diverged', path
    for path in manifest.submodules:
        a, b = core.links.get(path), port.links.get(path)
        if a != b:
            yield 'submodule', path


def show_diff(core, port, path, label):
    def lines(tree):
        if path not in tree.blobs:
            return []
        return tree.read(path).decode('utf-8', errors='replace').splitlines(keepends=True)
    a, b = lines(core), lines(port)
    if b'\0' in b''.join(x.encode() for x in a + b)[:8192]:
        print(f'Binary files ps4_core/{path} and {label}/{path} differ')
        return
    sys.stdout.writelines(difflib.unified_diff(a, b, f'ps4_core/{path}', f'{label}/{path}'))


PORTS = ('tlg_pc', 'gr2_pc', 'infamous_pc')
STATES = ('port-newer', 'port-only', 'diverged', 'core-newer', 'core-only', 'submodule')


def status_all(manifest, ref, fetch, ports):
    """One Markdown table row per port: what ps4_core would take (port-newer, port-only,
    diverged) and what the port is missing (core-newer, core-only, submodule)."""
    core = Tree(CORE)
    ancestors = lineage()
    head = git(CORE, 'rev-parse', '--short', 'HEAD').decode().strip()
    dirty = ' (+ uncommitted changes)' if git(CORE, 'status', '--porcelain').strip() else ''
    print(f'ps4_core {head}{dirty} vs <port>@{ref}\n')
    print('| Port | Checked out | ' + ref + ' | Differ | ' + ' | '.join(STATES) + ' |')
    print('|---|---|---|---|' + '---|' * len(STATES))
    for repo in ports or [CORE.parent / name for name in PORTS]:
        repo = repo.expanduser().resolve()
        if not (repo / '.git').exists():
            print(f'| {repo.name} | (missing) | | | ' + ' | ' * (len(STATES) - 1) + ' |')
            continue
        if fetch:
            git(repo, 'fetch', '--quiet', 'origin', check=False)
        branch = git(repo, 'branch', '--show-current', check=False).decode().strip() or 'detached'
        commit = git(repo, 'rev-parse', '--short', ref, check=False).decode().strip() or '?'
        if commit == '?':
            print(f'| {repo.name} | {branch} | (no {ref}) | | ' + ' | ' * (len(STATES) - 1) + ' |')
            continue
        counts = dict.fromkeys(STATES, 0)
        for status, _ in compare(Tree(repo, ref), ref, manifest, core, ancestors):
            counts[status] += 1
        cells = ' | '.join(str(counts[k] or '·') for k in STATES)
        print(f'| {repo.name} | {branch} | `{commit}` | {sum(counts.values())} | {cells} |')
    print('\nport-newer/port-only/diverged: the port has core work ps4_core lacks (--pull, merge diverged by hand).'
          '\ncore-newer/core-only/submodule: the port is behind ps4_core.')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('ports', nargs='*', type=Path, help='port repository (e.g. ~/decomp/tlg_pc)')
    p.add_argument('--ref', help='compare a branch/commit of the port instead of its working tree')
    p.add_argument('--diff', action='store_true', help='show a unified diff (ps4_core -> port)')
    p.add_argument('--pull', action='store_true', help="copy the port's newer core files into ps4_core")
    p.add_argument('--force', action='store_true', help='with --pull: also take diverged files')
    p.add_argument('--summary', action='store_true', help='only the counts per port')
    p.add_argument('--files', action='store_true', help="print the port's core file list (by the manifest) and exit")
    p.add_argument('--status-all', action='store_true',
                   help=f'sync table of the known ports ({", ".join(PORTS)}) against --ref (default origin/main)')
    p.add_argument('--fetch', action='store_true', help='with --status-all: git fetch origin in each port first (never pushes)')
    args = p.parse_args()

    manifest = Manifest(CORE / 'CORE_MANIFEST')
    if args.status_all:
        status_all(manifest, args.ref or 'origin/main', args.fetch, args.ports)
        return
    if not args.ports:
        p.error('give at least one port repository (or --status-all)')
    if args.files:
        for port_path in args.ports:
            tree = Tree(port_path, args.ref)
            print('\n'.join(sorted(x for x in tree.blobs if manifest.matches(x))))
        return
    core = Tree(CORE)
    ancestors = lineage()
    for port_path in args.ports:
        port = Tree(port_path.expanduser().resolve(), args.ref)
        label = port.repo.name + (f'@{args.ref}' if args.ref else '')
        found = list(compare(port, args.ref, manifest, core, ancestors))
        counts = {}
        for status, _ in found:
            counts[status] = counts.get(status, 0) + 1
        detail = ', '.join(f'{k} {v}' for k, v in sorted(counts.items())) or 'identical'
        print(f'{label}: {len(found)} core files differ ({detail})')
        if args.summary:
            continue
        for status, path in found:
            print(f'  {status:11} {path}')
            if args.diff and status != 'submodule':
                show_diff(core, port, path, label)
        if args.pull:
            take = {'port-newer', 'port-only'} | ({'diverged'} if args.force else set())
            for status, path in found:
                if status in take:
                    target = CORE / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(port.read(path))
                    if path in port.executable:
                        target.chmod(target.stat().st_mode | 0o111)
                    print(f'  pulled      {path}')
                elif status == 'submodule':
                    print(f'  update with: git -C {CORE / path} fetch {port.repo / path} && '
                          f'git -C {CORE / path} checkout {port.links.get(path)}')
            print('Review with `git -C ~/decomp/ps4_core diff` and commit by hand.')


if __name__ == '__main__':
    main()
