#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Reads a 48 kHz float stereo WAV recorded by measure-unmute.sh while
sink_probe played toneclick.mp3, and says for every jump where the sound
started relative to the click: how many ms of the tone before the cue
were heard, or that the click was not heard at all (the unmute came
after it, or there was no pre-roll)."""
import struct
import sys

RATE = 48000

raw = open(sys.argv[1], 'rb').read()
data = raw[raw.find(b'data') + 8:]
x = struct.unpack('<%df' % (len(data) // 8 * 2), data[:len(data) // 8 * 8])[0::2]

# A jump is a run of sound, jumps are 100 ms or more of silence apart.
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

early = []
cut = 0
for s, e in segments:
    peaks = [k for k in range(s, e + 1) if abs(x[k]) > 0.6]
    if not peaks:
        cut += 1
        print('jump: click NOT heard (%.0f ms of sound)' % ((e - s) * 1000 / RATE))
        continue
    ms = (peaks[0] - s) * 1000 / RATE
    early.append(ms)
    print('jump: sound from %.2f ms before the click' % ms)
summary = 'SUMMARY jumps=%d click-not-heard=%d' % (len(segments), cut)
if early:
    early.sort()
    summary += ' heard-before-cue: min %.1f median %.1f max %.1f ms' % (early[0], early[len(early) // 2], early[-1])
print(summary)
