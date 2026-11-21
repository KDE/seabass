#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Reads a 48 kHz float stereo WAV recorded by measure-unmute.sh while
zero_probe played a click file from 0, and says for every run how
many ms after the probe's marker (a 2 ms step to -0.9) the file's next
click was heard. The same number for the MP3 and for the WAV decoded
from it means position() stands for the same sound in both. The summary
ends with how many runs had each value, rounded to 0.1 ms.

    analyse_zero.py RECORDING.wav"""
import collections
import sys

from analyse_common import MS, read_left, spread

MARKER = -0.7
# The marker is negative; the click is found by its positive peak, which
# is 0.47 in the decoded click-D files and over 0.9 in the toneclick ones.
CLICK_UP = 0.3
WITHIN = 500 * MS

x = read_left(sys.argv[1])
distances = []
markers = 0
k = 0
n = len(x)
while k < n:
    if x[k] >= MARKER:
        k += 1
        continue
    markers += 1
    click = next((j for j in range(k + 5 * MS, min(n, k + WITHIN)) if x[j] > CLICK_UP), None)
    if click is None:
        print('run: no click within 500 ms of the marker')
    else:
        distance = (click - k) / MS
        distances.append(distance)
        print('run: click %.2f ms after the marker' % distance)
    k += WITHIN
summary = 'SUMMARY markers=%d measured=%d' % (markers, len(distances))
if distances:
    counts = collections.Counter('%.1f' % v for v in distances)
    summary += ' marker to click: %s ms | %s' % (
        spread(distances), ', '.join('%s: %d' % kv for kv in sorted(counts.items(), key=lambda kv: float(kv[0]))))
print(summary)
