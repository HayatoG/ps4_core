#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compares two NGS2 mixes recorded as float WAV (TLG_NGS2_TRACE_WAV with the console module,
TLG_NGS2_WAV with the built-in mixer): per-channel level, how much of the time each channel is
active, and the spectral balance, over the whole file or a window.

    ngs2_compare.py native.wav builtin.wav [--start S] [--seconds N]
    ngs2_compare.py native.wav builtin.wav --align [--start S] [--seconds N]

Without --align the two runs are compared as statistics. With --align the second recording is
lined up with the first (log-envelope cross-correlation, then sample cross-correlation): the
title screen of the same game plays the same streams, so this gives per-channel level
differences and waveform correlation of the same material. Large level differences point at volume/matrix bugs; a channel silent in one mix and
not in the other at routing; a shifted spectrum at pitch/resampling or missing filters.
"""
import argparse
import struct
import sys

import numpy as np

NAMES = ['FL', 'FR', 'FC', 'LFE', 'SL', 'SR', 'BL', 'BR']


def read_wav(path, start, seconds):
    with open(path, 'rb') as f:
        if f.read(4) != b'RIFF':
            sys.exit(f'{path}: not a WAV file')
        f.read(8)
        channels = rate = 0
        while True:
            head = f.read(8)
            if len(head) < 8:
                sys.exit(f'{path}: no data chunk')
            tag, size = head[:4], struct.unpack('<I', head[4:])[0]
            if tag == b'fmt ':
                fmt = f.read(size)
                kind, channels, rate = struct.unpack('<HHI', fmt[:8])
                if kind != 3:
                    sys.exit(f'{path}: expected float samples')
            elif tag == b'data':
                f.seek(int(start * rate) * channels * 4, 1)
                count = int(seconds * rate) * channels if seconds else -1
                data = np.fromfile(f, dtype='<f4', count=count)
                frames = len(data) // channels
                return data[:frames * channels].reshape(frames, channels), rate
            else:
                f.seek(size, 1)


def describe(name, x, rate):
    print(f'{name}: {len(x) / rate:.1f} s, {x.shape[1]} channels')
    window = rate // 10
    blocks = len(x) // window
    for c in range(x.shape[1]):
        ch = x[:blocks * window, c]
        rms = np.sqrt(np.mean(ch * ch)) if len(ch) else 0.0
        peak = np.max(np.abs(ch)) if len(ch) else 0.0
        active = np.mean(np.sqrt(np.mean(ch.reshape(blocks, window) ** 2, axis=1)) > 1e-3) if blocks else 0.0
        spectrum = np.abs(np.fft.rfft(ch[:rate * 8])) if len(ch) >= rate else None
        centroid = 0.0
        if spectrum is not None and spectrum.sum() > 0:
            freqs = np.fft.rfftfreq(min(len(ch), rate * 8), 1.0 / rate)
            centroid = float((freqs * spectrum).sum() / spectrum.sum())
        label = NAMES[c] if c < len(NAMES) else str(c)
        print(f'  {label:>3}: rms {20 * np.log10(rms + 1e-12):6.1f} dB  peak {peak:5.2f}  active {active * 100:5.1f}%  '
              f'centroid {centroid:6.0f} Hz')


def envelope(x, window):
    n = len(x) // window
    return np.log(np.sqrt((x[:n * window].reshape(n, window) ** 2).mean(axis=1)) + 1e-6)


def align(path_a, path_b, start, seconds):
    a, rate = read_wav(path_a, 0, start + seconds)
    b, _ = read_wav(path_b, 0, 0)
    window = rate // 100
    ea, eb = envelope(a[:, 0], window), envelope(b[:, 0], window)
    seg = ea[int(start * 100):]
    best = (-2.0, 0)
    first = max(0, int(start * 100) - 3000)   # the two runs start within 30 s of each other
    with np.errstate(invalid='ignore', divide='ignore'):
        for lag in range(first, max(first + 1, min(len(eb) - len(seg), int(start * 100) + 3000))):
            c = np.corrcoef(seg, eb[lag:lag + len(seg)])[0, 1]
            if c > best[0]:
                best = (c, lag)
    offset = best[1] * window - int(start * rate)
    print(f'envelope match {best[0]:.3f}: {path_b} runs {offset / rate:+.3f} s from {path_a}')
    # Refine to the sample on 4 s of the first channel.
    x = a[int(start * rate):int(start * rate) + 4 * rate, 0]
    lo = int(start * rate) + offset - rate
    y = b[max(0, lo):max(0, lo) + 6 * rate, 0]
    size = 1 << int(np.ceil(np.log2(len(x) + len(y))))
    cc = np.fft.irfft(np.fft.rfft(y, size) * np.conj(np.fft.rfft(x, size)), size)[:len(y) - len(x)]
    k = int(np.argmax(cc)) + max(0, lo) - int(start * rate)
    n = min(len(a) - int(start * rate), len(b) - int(start * rate) - k, int(seconds * rate))
    if n <= 0:
        sys.exit('no overlap after alignment')
    xa = a[int(start * rate):int(start * rate) + n]
    xb = b[int(start * rate) + k:int(start * rate) + k + n]
    print(f'sample offset {k} ({k / rate:+.4f} s), {n / rate:.1f} s compared')
    for c in range(min(xa.shape[1], xb.shape[1])):
        ra, rb = np.sqrt(np.mean(xa[:, c] ** 2)), np.sqrt(np.mean(xb[:, c] ** 2))
        if ra < 1e-7 and rb < 1e-7:
            continue
        corr = np.corrcoef(xa[:, c], xb[:, c])[0, 1] if ra > 1e-7 and rb > 1e-7 else 0.0
        gain = np.dot(xa[:, c], xb[:, c]) / max(np.dot(xa[:, c], xa[:, c]), 1e-12)
        label = NAMES[c] if c < len(NAMES) else str(c)
        print(f'  {label:>3}: level {20 * np.log10((rb + 1e-12) / (ra + 1e-12)):+6.2f} dB  waveform correlation {corr:6.3f}  gain {gain:6.3f}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('files', nargs='+')
    parser.add_argument('--start', type=float, default=0.0, help='seconds to skip')
    parser.add_argument('--seconds', type=float, default=0.0, help='seconds to read (0 = all)')
    parser.add_argument('--align', action='store_true', help='line the second file up with the first and compare')
    args = parser.parse_args()
    if args.align:
        if len(args.files) != 2:
            sys.exit('--align takes two files')
        align(args.files[0], args.files[1], args.start, args.seconds or 30.0)
        return
    for path in args.files:
        x, rate = read_wav(path, args.start, args.seconds)
        describe(path, x, rate)


if __name__ == '__main__':
    main()
