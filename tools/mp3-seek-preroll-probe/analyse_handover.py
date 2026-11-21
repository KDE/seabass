#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Reads a 48 kHz float stereo WAV recorded by measure-unmute.sh while
handover_probe or switch_probe played a toneclick file (a 440 Hz tone at
0.15 under clicks one second apart), and says for every jump:

  before   ms of sound before the first click (handover_probe: 0 means
           nothing before the cue)
  offset   second click minus first click minus 1000 ms: how much later
           the second stream (the player's, or the one switched to)
           sounds than the first for the same track time
  gap      ms between the clicks where nothing sounded (1 ms blocks whose
           peak is under GAP, half the tone)
  off      ms between the clicks whose peak is not the tone's (0.11 to
           0.19): the gap, plus any stretch where both streams sounded

A jump with fewer than two clicks is listed and counted as broken. The
summary ends with how many jumps had each offset and each gap, rounded to
0.1 ms and 1 ms. Leaks are listed and counted apart (analyse_common.py).

    analyse_handover.py RECORDING.wav"""
import collections
import sys

from analyse_common import MS, clicks, jumps_and_leaks, print_leaks, read_left, spread

GAP = 0.075

x = read_left(sys.argv[1])
jumps, leaks = jumps_and_leaks(x)

offsets = []
gaps = []
offs = []
befores = []
broken = 0
for s, e in jumps:
    c = clicks(x, s, e)
    if len(c) < 2:
        broken += 1
        print('jump: broken, %d click(s) heard, %.0f ms of sound' % (len(c), (e - s) / MS))
        continue
    before = (c[0] - s) / MS
    offset = (c[1] - c[0]) / MS - 1000.0
    gap = 0
    off = 0
    for b in range(c[0] + 3 * MS, c[1] - 3 * MS, MS):
        peak = max(max(x[b:b + MS]), -min(x[b:b + MS]))
        if peak < GAP:
            gap += 1
        if peak < 0.11 or peak > 0.19:
            off += 1
    befores.append(before)
    offsets.append(offset)
    gaps.append(gap)
    offs.append(off)
    print('jump: before %.2f ms, offset %.2f ms, gap %d ms, off %d ms' % (before, offset, gap, off))
print_leaks(leaks)


def histogram(values, fmt):
    counts = collections.Counter(fmt % v for v in values)
    return ', '.join('%s: %d' % (k, n) for k, n in sorted(counts.items(), key=lambda kv: float(kv[0])))


summary = 'SUMMARY jumps=%d measured=%d broken=%d leaks=%d' % (len(jumps), len(offsets), broken, len(leaks))
if offsets:
    summary += ' | before: %s | offset: %s | gap: %s | off: %s ms | offsets %s | gaps %s' % (
        spread(befores), spread(offsets), spread(gaps), spread(offs),
        histogram(offsets, '%.1f'), histogram(gaps, '%d'))
print(summary)
