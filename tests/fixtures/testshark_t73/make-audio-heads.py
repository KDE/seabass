#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: CC0-1.0
"""Cut the T73 click files down to their heads: every byte a tag reader
or Seabass's MP3 lead-in reader looks at, and a few KB of audio.

    make-audio-heads.py SOURCE_DIR OUT_DIR

FLAC: the metadata blocks (STREAMINFO keeps the full sample count, so the
length still reads 180 or 240 s) and the first 4 KB of frames.
AIFF: every chunk, SSND cut to 4 KB of sound with its size fixed and FORM's
size fixed after it (COMM keeps the full frame count).
MP3: the first 32 KB (ID3, the Info frame with its LAME tag and frame
count, and the first frames).

The files were made for #73 with ffmpeg (see README.md); the stick held
byte-identical copies, which rekordbox 7.2.18 exported.
"""
import pathlib
import struct
import sys

KEEP = 4096


def flac_head(data):
    assert data[:4] == b"fLaC"
    at = 4
    while True:
        header = data[at]
        size = int.from_bytes(data[at + 1:at + 4], "big")
        at += 4 + size
        if header & 0x80:
            break
    return data[:at + KEEP]


def aiff_head(data):
    assert data[:4] == b"FORM" and data[8:12] in (b"AIFF", b"AIFC")
    out = bytearray(data[:12])
    at = 12
    while at + 8 <= len(data):
        cid = data[at:at + 4]
        size = int.from_bytes(data[at + 4:at + 8], "big")
        body = data[at + 8:at + 8 + size]
        if cid == b"SSND":
            body = body[:8 + KEEP]  # offset, blockSize, then sound
        out += cid + struct.pack(">I", len(body)) + body
        if len(body) % 2:
            out += b"\0"
        at += 8 + size + (size % 2)
    out[4:8] = struct.pack(">I", len(out) - 8)
    return bytes(out)


def mp3_head(data):
    return data[:32768]


def main():
    source, target = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    target.mkdir(parents=True, exist_ok=True)
    cut = {".flac": flac_head, ".aiff": aiff_head, ".mp3": mp3_head}
    for file in sorted(source.iterdir()):
        if file.suffix in cut:
            (target / file.name).write_bytes(cut[file.suffix](file.read_bytes()))


if __name__ == "__main__":
    main()
