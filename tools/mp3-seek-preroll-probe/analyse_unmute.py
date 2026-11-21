#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Reads a 48 kHz float stereo WAV recorded by measure-unmute.sh while
sink_probe played a click file (toneclick*.mp3, or a WAV decoded from
one), and says for every jump where the sound started relative to the
click: how many ms of what comes before the cue were heard, or that the
click was not heard at all (the unmute came after it, or there was no
pre-roll). The summary ends with how many jumps had each value, rounded
to 0.1 ms. Leaks (short sound while the output was meant to be muted,
see analyse_common.py) are listed and counted apart.

    analyse_unmute.py RECORDING.wav"""
import collections
import sys

from analyse_common import MS, clicks, jumps_and_leaks, print_leaks, read_left, spread

x = read_left(sys.argv[1])
jumps, leaks = jumps_and_leaks(x)

early = []
cut = 0
for s, e in jumps:
    found = clicks(x, s, e)
    if not found:
        cut += 1
        print('jump: click NOT heard (%.0f ms of sound)' % ((e - s) / MS))
        continue
    ms = (found[0] - s) / MS
    early.append(ms)
    print('jump: sound from %.2f ms before the click' % ms)
print_leaks(leaks)
summary = 'SUMMARY jumps=%d click-not-heard=%d leaks=%d' % (len(jumps), cut, len(leaks))
if early:
    counts = collections.Counter('%.1f' % v for v in early)
    summary += ' heard-before-cue: %s ms | %s' % (
        spread(early), ', '.join('%s: %d' % kv for kv in sorted(counts.items(), key=lambda kv: float(kv[0]))))
print(summary)
