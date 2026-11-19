#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Writes the probe audio for #92 into OUT_DIR (default: the current one).

click.wav       4 s of digital silence, a 1.1 ms click at 0.9 of full
                scale at 1.5, 2.5 and 3.5 s, 44.1 kHz stereo
click.flac/.m4a/.mp3   the same through ffmpeg (FLAC, AAC 192k, LAME 192k)
toneclick.wav   the same clicks over a 440 Hz tone at 0.15: anything
                that sounds before a click shows as tone in a recording
toneclick.mp3   the same through LAME 192k

Needs the ffmpeg command line with libmp3lame.
"""
import math
import os
import struct
import subprocess
import sys
import wave

RATE = 44100
CLICKS = (1.5, 2.5, 3.5)


def write(path, tone):
    frames = bytearray()
    for i in range(RATE * 4):
        t = i / RATE
        v = 0.15 * math.sin(2 * math.pi * 440 * t) if tone else 0.0
        if any(c <= t < c + 0.0011 for c in CLICKS):
            v = 0.9 if not tone else 0.95
        s = int(max(-1.0, min(1.0, v)) * 32767)
        frames += struct.pack('<hh', s, s)
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


def ffmpeg(src, dst, *codec):
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', src, *codec, dst], check=True)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    click = os.path.join(out, 'click.wav')
    tone = os.path.join(out, 'toneclick.wav')
    write(click, tone=False)
    write(tone, tone=True)
    ffmpeg(click, os.path.join(out, 'click.flac'))
    ffmpeg(click, os.path.join(out, 'click.m4a'), '-c:a', 'aac', '-b:a', '192k')
    ffmpeg(click, os.path.join(out, 'click.mp3'), '-c:a', 'libmp3lame', '-b:a', '192k')
    ffmpeg(tone, os.path.join(out, 'toneclick.mp3'), '-c:a', 'libmp3lame', '-b:a', '192k')


if __name__ == '__main__':
    main()
