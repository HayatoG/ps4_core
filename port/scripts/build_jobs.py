#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""build_jobs.py <xpps folder> <out.jobs> [--index <sqlite>] [--jobs N] [--nice] [--compute-wide]

Runs the shader jobs generator (tools/xpps, `python -m xpps.jobs build`) from the launcher: with
python in a source tree, inside the packaged executable (--script) in a package, where there is no
python. Prints the generator's `PROGRESS <phase> <n> <total>` lines; exit code 0 on success.
"""
import multiprocessing
from pathlib import Path
import sys

TOOLS = Path(__file__).resolve().parent.parent / 'tools'


def main(argv):
    sys.path.insert(0, str(TOOLS))
    from xpps import jobs
    return jobs.main(['build', *argv])


if __name__ == '__main__':
    multiprocessing.freeze_support()   # the index phase starts worker processes
    sys.exit(main(sys.argv[1:]))
