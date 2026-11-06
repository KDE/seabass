#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Write the click MP3s (#50) into tests/fixtures/mp3-click/.

Three seconds of digital silence with a one-sample full-scale click at
exactly 1.500 s (sample 66150 at 44.1 kHz) and a click at a quarter of
full scale at 2.500 s, encoded once per mp3guessenc class so the same
audio instant can be found in every decoder's output:

  A  CBR without an Info frame (libmp3lame with the tag frame off)
  B  the D file with its 9-byte encoder string zeroed, nothing else
     changed: an Info frame whose LAME extension names no encoder
  B  FFmpeg's own Info frame ("Lavc..."), stereo over joint stereo audio
  C  the D file with its LAME tag CRC broken
  D  LAME with a valid tag, at 44.1 kHz, 48 kHz, MPEG-2 22.05 kHz, mono

LAME files come from libmp3lame through lame_encode.encode(), the Lavc
file from the ffmpeg command line over a WAV of the same samples. Every
file is CBR: 64 kbit/s, except 80 at 48 kHz, where a 64 kbit/s frame is
too short for libmp3lame to write the tag frame at all.

usage: make-click-mp3s.py [OUT_DIR]
"""

import os
import struct
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lame_encode  # noqa: E402

SECONDS = 3
FULL_SCALE = 32767
QUIET = 8192  # 0.25 of full scale


def click_pcm(rate):
    """Silence with the full-scale click at 1.5 s and the quiet one at 2.5 s."""
    pcm = [0] * (SECONDS * rate)
    pcm[rate * 3 // 2] = FULL_SCALE
    pcm[rate * 5 // 2] = QUIET
    return pcm


def write_wav(path, rate, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % (2 * len(pcm)), *[v for v in pcm for _ in (0, 1)]))


def encoder_string_offset(data):
    """Offset of the 9-byte encoder string in the leading Xing/Info frame."""
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


def lame(out, rate, channels, mode, kbps, notag=False):
    lame_encode.encode(out, rate, channels, mode, str(kbps), notag, click_pcm(rate))


def patched(src, out, patch):
    data = bytearray(open(src, "rb").read())
    patch(data, encoder_string_offset(data))
    open(out, "wb").write(data)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "tests", "fixtures", "mp3-click")
    os.makedirs(out, exist_ok=True)
    p = lambda name: os.path.join(out, name)

    lame(p("click-A-notag-44100.mp3"), 44100, 2, "joint", 64, notag=True)
    lame(p("click-D-lame-44100.mp3"), 44100, 2, "joint", 64)
    lame(p("click-D-lame-48000.mp3"), 48000, 2, "joint", 80)
    lame(p("click-D-lame-mpeg2-22050.mp3"), 22050, 2, "joint", 64)
    lame(p("click-D-lame-mono-44100.mp3"), 44100, 1, "mono", 64)

    def zero_encoder_string(data, at):
        data[at:at + 9] = bytes(9)

    def break_crc(data, at):
        # The tag CRC is the last two bytes of the LAME extension, 34
        # bytes after the encoder string; its second byte is inverted.
        data[at + 35] ^= 0xFF

    patched(p("click-D-lame-44100.mp3"), p("click-B-noencoder-44100.mp3"), zero_encoder_string)
    patched(p("click-D-lame-44100.mp3"), p("click-C-badcrc-44100.mp3"), break_crc)

    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "click-44100.wav")
        write_wav(wav, 44100, click_pcm(44100))
        subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", wav,
                        "-c:a", "libmp3lame", "-b:a", "64k", p("click-B-lavc-44100.mp3")], check=True)

    for name in sorted(os.listdir(out)):
        if name.endswith(".mp3"):
            print("%7d  %s" % (os.path.getsize(p(name)), name))


if __name__ == "__main__":
    main()
