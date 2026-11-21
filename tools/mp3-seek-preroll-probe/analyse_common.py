# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""What analyse_unmute.py and analyse_handover.py share: reading a
recording and cutting it into jumps.

A recording is the 32-bit float stereo WAV pw-record writes at 48 kHz;
only the left channel is read, into an array of floats (4 bytes a
sample, not a Python float each).

A jump is a run of sound; runs 100 ms or more of silence apart are
separate. A run shorter than LEAK_MS that holds no click is not a jump
but a leak: sound that came out while the probe had the output muted or
silent (it is counted and listed apart, never as a jump)."""
import array
import struct
import sys

RATE = 48000
MS = RATE // 1000
SOUND = 0.02        # above this a sample is sound
CLICK = 0.6         # above this a sample is a click (the tone is 0.15)
LEAK_MS = 50


def read_left(path):
    raw = open(path, 'rb').read()
    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        sys.exit('%s is not a WAV' % path)
    pos = 12
    fmt = None
    data = None
    while pos + 8 <= len(raw):
        tag, size = raw[pos:pos + 4], struct.unpack('<I', raw[pos + 4:pos + 8])[0]
        body = raw[pos + 8:pos + 8 + size]
        if tag == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif tag == b'data':
            data = body
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        sys.exit('%s has no fmt or data chunk' % path)
    code, channels, rate, _, _, bits = fmt
    if code not in (3, 0xFFFE) or bits != 32 or channels != 2 or rate != RATE:
        sys.exit('%s is not 48 kHz float stereo (format %d, %d ch, %d Hz, %d bit)'
                 % (path, code, channels, rate, bits))
    samples = array.array('f')
    samples.frombytes(data[:len(data) // 8 * 8])
    if sys.byteorder != 'little':
        samples.byteswap()
    return samples[0::2]


def runs(x):
    """(start, last) sample index of every run of sound."""
    found = []
    start = None
    quiet = 0
    last = 0
    for k, v in enumerate(x):
        if v > SOUND or v < -SOUND:
            if start is None:
                start = k
            quiet = 0
            last = k
        elif start is not None:
            quiet += 1
            if quiet > RATE // 10:
                found.append((start, last))
                start = None
    if start is not None:
        found.append((start, last))
    return found


def clicks(x, s, e):
    found = []
    k = s
    while k <= e:
        if x[k] > CLICK or x[k] < -CLICK:
            found.append(k)
            k += 50 * MS
        else:
            k += 1
    return found


def jumps_and_leaks(x):
    """The jumps, and the leaks as (time in s, length in ms, peak)."""
    jumps = []
    leaks = []
    for s, e in runs(x):
        if (e - s) < LEAK_MS * MS and not clicks(x, s, e):
            leaks.append((s / RATE, (e - s) / MS, max(abs(v) for v in x[s:e + 1])))
        else:
            jumps.append((s, e))
    return jumps, leaks


def spread(values):
    values = sorted(values)
    return 'min %.1f median %.1f max %.1f' % (values[0], values[len(values) // 2], values[-1])


def print_leaks(leaks):
    for at, length, peak in leaks:
        print('leak: %.1f ms of sound at %.3f s, peak %.3f' % (length, at, peak))
