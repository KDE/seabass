#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Write the beat-click MP3s (#50): what rekordbox and Engine DJ analyse
to show their own timelines, without anyone placing a cue by hand.

Sixty seconds of digital silence with a click every 500 ms (120 BPM),
the first one at exactly 1.000 s, every fourth louder. Each click is a
2 ms burst of a 2 kHz tone that starts on its sample and dies away, so a
beat analyser locks onto its onset. Encoded once per mp3guessenc class
as make-click-mp3s.py does:

  A  CBR without an Info frame
  B  the D file with its encoder string zeroed
  B  FFmpeg's own "Lavc" Info frame
  C  the D file with its LAME tag CRC broken
  D  LAME with a valid tag, 44.1 kHz; and the same at 48 kHz

The first beat each app writes is 1000 ms plus that app's lead on that
file plus the analyser's own bias. The bias is the same for every file,
so the rekordbox minus Engine first-beat difference per file, less the
class A difference, is the measurement. Predicted at 44.1 kHz: A 0,
B zeroed 26.1 ms, B Lavc 25.1 ms, C and D 51.2 ms (47.0 at 48 kHz).

With quantize on, a hot cue pressed near the first beat snaps onto it in
both apps, which puts the same instant into the cue tables too.

usage: make-beat-click-mp3s.py OUT_DIR
"""

import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lame_encode  # noqa: E402

SECONDS = 60
BPM = 120
FIRST_BEAT_S = 1.0
FULL_SCALE = 32767
ACCENT = 1.0
PLAIN = 0.5
BURST_S = 0.002
TONE_HZ = 2000.0


def beat_click_pcm(rate):
    pcm = [0] * (SECONDS * rate)
    burst = int(BURST_S * rate)
    beat_s = 60.0 / BPM
    beat = 0
    while True:
        start = FIRST_BEAT_S + beat * beat_s
        at = int(round(start * rate))
        if at + burst >= len(pcm):
            break
        level = ACCENT if beat % 4 == 0 else PLAIN
        for i in range(burst):
            decay = math.exp(-6.0 * i / burst)
            pcm[at + i] = int(FULL_SCALE * level * decay * math.cos(2 * math.pi * TONE_HZ * i / rate))
        beat += 1
    return pcm


def write_wav(path, rate, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % (2 * len(pcm)), *[v for v in pcm for _ in (0, 1)]))


def encoder_string_offset(data):
    for magic in (b"Info", b"Xing"):
        at = data.find(magic, 0, 64)
        if at >= 0:
            break
    else:
        sys.exit("no Xing/Info frame")
    flags = struct.unpack(">I", data[at + 4:at + 8])[0]
    at += 8
    for bit, size in ((0x1, 4), (0x2, 4), (0x4, 100), (0x8, 4)):
        if flags & bit:
            at += size
    return at


def patched(src, out, patch):
    data = bytearray(open(src, "rb").read())
    patch(data, encoder_string_offset(data))
    open(out, "wb").write(data)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    p = lambda name: os.path.join(out, name)
    pcm44 = beat_click_pcm(44100)
    pcm48 = beat_click_pcm(48000)

    lame_encode.encode(p("beats-A-notag-44100.mp3"), 44100, 2, "joint", "64", True, pcm44)
    lame_encode.encode(p("beats-D-lame-44100.mp3"), 44100, 2, "joint", "64", False, pcm44)
    lame_encode.encode(p("beats-D-lame-48000.mp3"), 48000, 2, "joint", "80", False, pcm48)

    def zero_encoder_string(data, at):
        data[at:at + 9] = bytes(9)

    def break_crc(data, at):
        data[at + 35] ^= 0xFF

    patched(p("beats-D-lame-44100.mp3"), p("beats-B-noencoder-44100.mp3"), zero_encoder_string)
    patched(p("beats-D-lame-44100.mp3"), p("beats-C-badcrc-44100.mp3"), break_crc)

    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "beats-44100.wav")
        write_wav(wav, 44100, pcm44)
        subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", wav,
                        "-c:a", "libmp3lame", "-b:a", "64k", p("beats-B-lavc-44100.mp3")], check=True)
    # The same audio as a WAV: both apps analyse it with no lead at all,
    # which shows each analyser's bias on its own.
    write_wav(p("beats-ref-44100.wav"), 44100, pcm44)

    for name in sorted(os.listdir(out)):
        print("%8d  %s" % (os.path.getsize(p(name)), name))


if __name__ == "__main__":
    main()
