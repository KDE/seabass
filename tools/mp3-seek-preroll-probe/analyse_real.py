#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""For jumps on real music, where there is no probe click to find: reads
a recording made by measure-unmute.sh (sink_probe plain jumps to CUE_MS)
and the 16-bit WAV pcm_probe decoded from the same track, and says for
every jump where in the track the first sound it played comes from,
relative to the cue:

  start  > 0: the jump's first sound is that many ms after the cue (the
         start of what the cue marks was lost); < 0: that many ms from
         before the cue were heard; 0: it starts on the cue

It compares the 1 ms peak envelope of the jump's first 250 ms with the
reference's at every offset within +-100 ms and takes the best match
(so it is good to about 1 ms), and gives the match's correlation. Leaks
are listed and counted apart (analyse_common.py).

    analyse_real.py RECORDING.wav REFERENCE.wav CUE_MS"""
import array
import struct
import sys

from analyse_common import MS, jumps_and_leaks, print_leaks, read_left, spread

x = read_left(sys.argv[1])
raw = open(sys.argv[2], 'rb').read()
if raw[36:40] != b'data':
    sys.exit('%s is not a WAV from pcm_probe' % sys.argv[2])
ref_rate = struct.unpack('<I', raw[24:28])[0]
ref = array.array('h')
ref.frombytes(raw[44:44 + (len(raw) - 44) // 4 * 4])
if sys.byteorder != 'little':
    ref.byteswap()
ref = ref[0::2]
cue = float(sys.argv[3])
LENGTH = 250
SEARCH = 100


def ref_env(ms):
    a = int(ms * ref_rate / 1000)
    b = int((ms + 1) * ref_rate / 1000)
    w = ref[a:b]
    return max(max(w), -min(w)) / 32768 if w else 0.0


def rec_env(start, ms):
    w = x[start + ms * MS:start + (ms + 1) * MS]
    return max(max(w), -min(w)) if w else 0.0


reference = {ms: ref_env(cue + ms) for ms in range(-SEARCH, SEARCH + LENGTH)}


def correlation(a, b):
    n = len(a)
    ma = sum(a) / n
    mb = sum(b) / n
    num = sum((p - ma) * (q - mb) for p, q in zip(a, b))
    da = sum((p - ma) ** 2 for p in a) ** 0.5
    db = sum((q - mb) ** 2 for q in b) ** 0.5
    return num / (da * db) if da and db else 0.0


jumps, leaks = jumps_and_leaks(x)
starts = []
for s, e in jumps:
    got = [rec_env(s, ms) for ms in range(LENGTH)]
    best = max(range(-SEARCH, SEARCH + 1),
               key=lambda lag: correlation(got, [reference[lag + ms] for ms in range(LENGTH)]))
    r = correlation(got, [reference[best + ms] for ms in range(LENGTH)])
    starts.append(best)
    print('jump: start %+d ms (correlation %.3f)' % (best, r))
print_leaks(leaks)
summary = 'SUMMARY jumps=%d leaks=%d' % (len(jumps), len(leaks))
if starts:
    summary += ' start: %s ms, on the cue: %d, after it: %d, before it: %d' % (
        spread(starts), sum(1 for v in starts if v == 0), sum(1 for v in starts if v > 0),
        sum(1 for v in starts if v < 0))
print(summary)
