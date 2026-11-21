#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Reads a 48 kHz float stereo WAV recorded by measure-handover.sh while
handover_probe played toneclick.mp3 (a 440 Hz tone at 0.15 under clicks
one second apart), and says for every jump:

  before   ms of sound before the first click (0: nothing before the cue)
  offset   second click minus first click minus 1000 ms: how much later
           the player's stream sounds than the probe's own sink
  gap      ms between the clicks where nothing sounded (1 ms blocks whose
           peak is under half the tone's)
  off      ms between the clicks whose peak is not the tone's (0.11 to
           0.19): the gap, plus any stretch where both streams sounded
"""
import struct
import sys

RATE = 48000
MS = RATE // 1000

raw = open(sys.argv[1], 'rb').read()
data = raw[raw.find(b'data') + 8:]
x = struct.unpack('<%df' % (len(data) // 8 * 2), data[:len(data) // 8 * 8])[0::2]

# A jump is a run of sound; jumps are 100 ms or more of silence apart.
segments = []
start = None
quiet = 0
last = 0
for k, v in enumerate(x):
    if abs(v) > 0.02:
        if start is None:
            start = k
        quiet = 0
        last = k
    elif start is not None:
        quiet += 1
        if quiet > RATE // 10:
            segments.append((start, last))
            start = None
if start is not None:
    segments.append((start, last))


def clicks(s, e):
    found = []
    k = s
    while k <= e:
        if abs(x[k]) > 0.6:
            found.append(k)
            k += 50 * MS
        else:
            k += 1
    return found


offsets = []
gaps = []
offs = []
befores = []
for s, e in segments:
    c = clicks(s, e)
    if len(c) < 2:
        print('jump: %d click(s) heard, %.0f ms of sound' % (len(c), (e - s) / MS))
        continue
    before = (c[0] - s) / MS
    offset = (c[1] - c[0]) / MS - 1000.0
    gap = 0
    off = 0
    for b in range(c[0] + 3 * MS, c[1] - 3 * MS, MS):
        peak = max(abs(v) for v in x[b:b + MS])
        if peak < 0.075:
            gap += 1
        if peak < 0.11 or peak > 0.19:
            off += 1
    befores.append(before)
    offsets.append(offset)
    gaps.append(gap)
    offs.append(off)
    print('jump: before %.2f ms, offset %.2f ms, gap %d ms, off %d ms' % (before, offset, gap, off))


def spread(values):
    values = sorted(values)
    return 'min %.1f median %.1f max %.1f' % (values[0], values[len(values) // 2], values[-1])


summary = 'SUMMARY jumps=%d measured=%d' % (len(segments), len(offsets))
if offsets:
    summary += ' | before: %s | offset: %s | gap: %s | off: %s ms' % (
        spread(befores), spread(offsets), spread(gaps), spread(offs))
print(summary)
