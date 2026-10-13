#!/usr/bin/env python3
"""Desempenho de uma sessão a partir de um log do port (TLG_FRAME_STATS=1, uma amostra a cada 5 s):
FPS, pior quadro e draws por quadro ao longo do tempo.
Uso: session_chart.py <log> <saída.png> [nome do jogo]"""
import re
import statistics
import sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

STAT = re.compile(r'Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms.*?(\d+) draws/frame')
PACE = re.compile(r'Frame pacing: median ([\d.]+) ms, stddev ([\d.]+) ms, p99 ([\d.]+) ms')

text = open(sys.argv[1], encoding='utf-8', errors='replace').read()
stats = [tuple(map(float, m.groups())) for m in STAT.finditer(text)]
pacing = [tuple(map(float, m.groups())) for m in PACE.finditer(text)]
# The first samples cover boot and shader warm-up; the chart shows the whole session anyway.
t = [i * 5 / 60 for i in range(len(stats))]
fps = [s[0] for s in stats]
worst = [s[1] for s in stats]
draws = [s[2] for s in stats]

ink, muted, accent, warn = '#1f2937', '#6b7280', '#2563eb', '#dc2626'
plt.rcParams.update({'font.size': 11, 'axes.edgecolor': '#d1d5db', 'axes.labelcolor': ink,
                     'xtick.color': muted, 'ytick.color': muted, 'axes.spines.top': False,
                     'axes.spines.right': False})
fig, (a, b, c) = plt.subplots(3, 1, figsize=(10, 6.4), sharex=True,
                              gridspec_kw={'height_ratios': [1.1, 1, 0.8]})
fig.patch.set_facecolor('white')
a.plot(t, fps, color=accent, lw=1.8)
a.axhline(30, color=muted, lw=0.8, ls='--')
a.set_ylim(0, 36)
a.set_ylabel('FPS')
a.text(t[-1], 31.2, 'trava do jogo: 30 FPS', ha='right', color=muted, fontsize=9)
b.plot(t, worst, color=warn, lw=1.4)
b.axhline(33.3, color=muted, lw=0.8, ls='--')
b.set_ylim(0, max(100, max(worst) * 1.1))
b.set_ylabel('pior quadro (ms)')
b.text(t[-1], 36, 'quadro ideal: 33,3 ms', ha='right', va='bottom', color=muted, fontsize=9)
c.plot(t, draws, color='#059669', lw=1.4)
c.set_ylabel('draws/quadro')
c.set_xlabel('minutos de sessão')
steady = [f for f in fps[6:]] or fps
at30 = sum(1 for f in steady if f >= 29.5) / len(steady) * 100
p99 = statistics.median(p[2] for p in pacing) if pacing else float('nan')
game = sys.argv[3] if len(sys.argv) > 3 else 'The Last Guardian'
fig.suptitle('%s no PC — sessão de %.0f min (Linux, Vulkan, RTX 2060 6 GB)' % (game, t[-1]),
             x=0.07, ha='left', color=ink, fontsize=13, fontweight='bold')
fig.text(0.07, 0.915, '%.0f%% das janelas de 5 s a 30 FPS · p99 do quadro %.1f ms · %d a %d draws por quadro'
         % (at30, p99, min(draws), max(draws)), color=muted, fontsize=10)
fig.tight_layout(rect=(0, 0, 1, 0.92))
fig.savefig(sys.argv[2], dpi=110)
print(f'{len(stats)} amostras, {at30:.0f}% a 30 FPS, p99 {p99:.2f} ms, draws {min(draws):.0f}-{max(draws):.0f}')
