#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Writes the probe audio for #92 into OUT_DIR (default: the current one).

    make-probe-audio.py [OUT_DIR] [--long]

click.wav       4 s of digital silence, a 1.1 ms click at 0.9 of full
                scale at 1.5, 2.5 and 3.5 s, 44.1 kHz stereo
click.flac/.m4a/.mp3   the same through ffmpeg (FLAC, AAC 192k, LAME 192k)
toneclick.wav   the same clicks over a 440 Hz tone at 0.15: anything
                that sounds before a click shows as tone in a recording
toneclick.mp3   the same through LAME 192k
toneclick-64k.mp3   the same through LAME 64k (a longer silent stretch
                after a seek: the reservoir reaches further back)
clickthentone.wav   digital silence up to the first click, the tone from
                there on: sound heard before a jump to the first click
                cannot come from before the cue in the file, so it is
                left over from the previous position
tonegap.wav     the tone and the clicks at 1.5 and 2.5 s, silence from
                2.6 s on: a jump made while playing in the silence shows
                any sound from before the cue as tone (sink_probe live)
tonegap.mp3     the same through LAME 192k
long-320k.mp3   (--long only) 6 minutes of a 440 Hz sine through LAME
                320k, for warm_cost and the decode timings

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


def write(path, tone, tone_from=0.0, sound_until=4.0):
    frames = bytearray()
    for i in range(RATE * 4):
        t = i / RATE
        v = 0.15 * math.sin(2 * math.pi * 440 * t) if tone and tone_from <= t < sound_until else 0.0
        if t < sound_until and any(c <= t < c + 0.0011 for c in CLICKS):
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
    args = [a for a in sys.argv[1:] if a != '--long']
    out = args[0] if args else '.'
    click = os.path.join(out, 'click.wav')
    tone = os.path.join(out, 'toneclick.wav')
    write(click, tone=False)
    write(tone, tone=True)
    ffmpeg(click, os.path.join(out, 'click.flac'))
    ffmpeg(click, os.path.join(out, 'click.m4a'), '-c:a', 'aac', '-b:a', '192k')
    ffmpeg(click, os.path.join(out, 'click.mp3'), '-c:a', 'libmp3lame', '-b:a', '192k')
    ffmpeg(tone, os.path.join(out, 'toneclick.mp3'), '-c:a', 'libmp3lame', '-b:a', '192k')
    ffmpeg(tone, os.path.join(out, 'toneclick-64k.mp3'), '-c:a', 'libmp3lame', '-b:a', '64k')
    write(os.path.join(out, 'clickthentone.wav'), tone=True, tone_from=CLICKS[0])
    gap = os.path.join(out, 'tonegap.wav')
    write(gap, tone=True, sound_until=2.6)
    ffmpeg(gap, os.path.join(out, 'tonegap.mp3'), '-c:a', 'libmp3lame', '-b:a', '192k')
    if '--long' in sys.argv:
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                        'sine=frequency=440:duration=360:sample_rate=44100', '-ac', '2',
                        '-c:a', 'libmp3lame', '-b:a', '320k', os.path.join(out, 'long-320k.mp3')], check=True)


if __name__ == '__main__':
    main()
