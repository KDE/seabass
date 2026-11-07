#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Put a Fraunhofer VBRI frame in front of an MP3 without a tag frame.

No encoder at hand writes VBRI (Fraunhofer's own encoders did), so the
frame is crafted: the first audio frame's header, 32 zero bytes of side
information, then at byte 36 the VBRI header (version 1, delay 0,
quality 0, the audio's byte and frame counts) and an empty seek table
(0 entries, scale 1, 2 bytes per entry, 1 frame per entry), padded with
zeros to the frame length the header gives.

With `modediffers` the VBRI frame's channel mode bits are set to stereo
(00), so over joint-stereo audio its mode is not the audio's.

usage: vbri_frame.py IN OUT [modediffers]
"""

import struct
import sys

BITRATES_V1 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
RATES_V1 = [44100, 48000, 32000]


def frame_length(h):
    """Length of an MPEG-1 Layer III frame with header bytes h."""
    if h[0] != 0xFF or (h[1] & 0xFE) != 0xFA:
        sys.exit("not an MPEG-1 Layer III frame header: %s" % h[:4].hex())
    bitrate = BITRATES_V1[h[2] >> 4] * 1000
    rate = RATES_V1[(h[2] >> 2) & 0x03]
    return 144 * bitrate // rate + ((h[2] >> 1) & 0x01)


def with_vbri(audio, mode_differs=False):
    """AUDIO (bytes, starting with a frame header) behind a VBRI frame."""
    frames = 0
    at = 0
    while at + 4 <= len(audio):
        at += frame_length(audio[at:at + 4])
        frames += 1
    header = bytearray(audio[:4])
    if mode_differs:
        header[3] &= 0x3F  # channel mode 00, stereo
    length = frame_length(header)
    frame = bytes(header) + bytes(32) + b"VBRI" \
        + struct.pack(">HHHII", 1, 0, 0, len(audio), frames) + struct.pack(">HHHH", 0, 1, 2, 1)
    return frame + bytes(length - len(frame)) + audio


def main():
    src, out = sys.argv[1:3]
    mode_differs = len(sys.argv) > 3 and sys.argv[3] == "modediffers"
    with open(src, "rb") as f:
        audio = f.read()
    with open(out, "wb") as f:
        f.write(with_vbri(audio, mode_differs))


if __name__ == "__main__":
    main()
