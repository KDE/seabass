#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#
# Writes the MP3 lead-in fixtures (#50) into tests/fixtures/mp3-lead-in/.
# The files are committed; this script is how they were made, so a new
# case is added here and the directory regenerated.
#
# LAME files come from libmp3lame through lame_encode.py (ctypes), the
# library the `lame` frontend wraps: the frontend is not installed on
# every machine that has the library (FFmpeg depends on it). Each is one
# second of silence with a click at half a second. FFmpeg files come from
# the ffmpeg command line.
#
# CBR needs 64 kbit/s or more: below that a frame is shorter than the
# 192-byte LAME tag and libmp3lame silently writes no Info frame at all.
#
# Usage:
#   tools/mp3-offset-probe/make-lead-in-fixtures.sh [OUT_DIR]

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$here/../../tests/fixtures/mp3-lead-in}"
mkdir -p "$out"
enc() { python3 -I "$here/lame_encode.py" "$@"; }

# LAME, MPEG-1 44.1 kHz joint stereo, CBR with its Info + LAME tag.
enc "$out/lame-cbr-44100.mp3" 44100 2 joint 64
# LAME VBR (-V 9): a Xing frame with the same tag.
enc "$out/lame-vbr-44100.mp3" 44100 2 joint v9
# LAME without the tag frame (the frontend's -t).
enc "$out/lame-notag-44100.mp3" 44100 2 joint 64 notag
# MPEG-2 Layer III: 576-sample frames.
enc "$out/lame-mpeg2-22050.mp3" 22050 2 joint 64
# 48 kHz.
enc "$out/lame-cbr-48000.mp3" 48000 2 joint 80
# Mono: 17 bytes of side information before the tag.
enc "$out/lame-mono-44100.mp3" 44100 1 mono 64
# CRC-protected frames (the frontend's -p): LAME still writes the tag at
# 4 + side-info bytes, not after the two CRC bytes, and so do FFmpeg and
# libmpg123 look for it there.
enc "$out/lame-crc-44100.mp3" 44100 2 joint 64 crc

# A crafted Fraunhofer VBRI frame in front of the file without a tag
# frame, once in the audio's channel mode and once stereo over the joint
# stereo audio (vbri_frame.py: no encoder at hand writes VBRI).
python3 -I "$here/vbri_frame.py" "$out/lame-notag-44100.mp3" "$out/vbri-44100.mp3"
python3 -I "$here/vbri_frame.py" "$out/lame-notag-44100.mp3" "$out/vbri-modediffers-44100.mp3" modediffers

# The CBR file with its LAME tag CRC broken: the CRC sits 34 bytes after
# the encoder string, at frame offset 190 here (header 4 + side info 32 +
# "Info" and flags 8 + frames, bytes, TOC, quality 112 + 34). Its second
# byte is replaced by its complement with dd.
variant() { python3 -I "$here/tag_frame_variants.py" "$@"; }
variant "$out/lame-cbr-44100.mp3" "$out/lame-junk-44100.mp3" junk
variant "$out/lame-cbr-44100.mp3" "$out/lame-id3junk-44100.mp3" id3junk
variant "$out/lame-cbr-44100.mp3" "$out/lame-id3id3-44100.mp3" id3id3
variant "$out/lame-cbr-44100.mp3" "$out/lame-nocounts-44100.mp3" nocounts
variant "$out/vbri-44100.mp3" "$out/vbri-nocounts-44100.mp3" nocounts

cp "$out/lame-cbr-44100.mp3" "$out/lame-badcrc-44100.mp3"
byte=$(od -An -tu1 -j191 -N1 "$out/lame-badcrc-44100.mp3" | tr -d ' ')
printf "\\x$(printf '%02x' $((255 - byte)))" \
    | dd of="$out/lame-badcrc-44100.mp3" bs=1 seek=191 count=1 conv=notrunc status=none

# The CBR file behind an ID3v2.4 tag with a footer (flag 0x10): a TIT2
# frame of 7 bytes, so a 17-byte body, then "3DI" repeating the header.
{
    printf 'ID3\x04\x00\x10\x00\x00\x00\x11'
    printf 'TIT2\x00\x00\x00\x07\x00\x00\x03footer'
    printf '3DI\x04\x00\x10\x00\x00\x00\x11'
    cat "$out/lame-cbr-44100.mp3"
} > "$out/lame-id3footer-44100.mp3"

# FFmpeg through libmp3lame: an ID3v2.4 tag, then FFmpeg's own Info frame
# ("Lavc..."), written stereo over joint stereo audio.
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "anullsrc=r=44100:cl=stereo" -t 1 \
    -c:a libmp3lame -b:a 64k "$out/ffmpeg-lavc-44100.mp3"

# Not an MP3 at all.
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "anullsrc=r=8000:cl=mono" -t 0.1 \
    -c:a pcm_s16le "$out/silence.wav"

ls -l "$out"
