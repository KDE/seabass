#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Variants of a LAME MP3 that move or change its Info frame (#50).

FFmpeg's mp3 demuxer looks for the Xing/Info/VBRI frame only at the
first byte after the ID3v2 tags, and uses it only when it says how many
frames the file has; libmpg123 resyncs over anything in front of the
first frame. These variants pin both behaviours:

  junk       100 zero bytes in front of the file
  id3junk    an ID3v2.3 tag with a 100-byte empty body, then 100 zero
             bytes, then the file
  id3id3     two ID3v2.3 tags back to back, each with a 100-byte empty
             body, then the file: the tag frame still first after the
             chain of tags
  nocounts   the tag frame's frame and byte count fields zeroed, their
             flag bits kept (a Xing/Info frame or a VBRI frame)
  l399       the LAME encoder string replaced by "L3.99r   ", the string
             LAME 3.99 alpha builds wrote; the tag CRC is recomputed so
             only the string differs

usage: tag_frame_variants.py IN OUT junk|id3junk|id3id3|nocounts|l399
"""

import struct
import sys


def xing_offset(data):
    """Offset of "Xing"/"Info" in the leading frame of DATA."""
    for magic in (b"Info", b"Xing"):
        at = data.find(magic, 0, 64)
        if at >= 0:
            return at
    sys.exit("no Xing/Info frame")


def encoder_string_offset(data):
    at = xing_offset(data)
    flags = struct.unpack(">I", data[at + 4:at + 8])[0]
    at += 8
    for bit, size in ((0x1, 4), (0x2, 4), (0x4, 100), (0x8, 4)):
        if flags & bit:
            at += size
    return at


def lame_crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def junk(data):
    return bytes(100) + data


def id3_tag(body=100):
    """An ID3v2.3 tag with BODY bytes of padding and no frames."""
    size = bytes([(body >> 21) & 0x7F, (body >> 14) & 0x7F, (body >> 7) & 0x7F, body & 0x7F])
    return b"ID3\x03\x00\x00" + size + bytes(body)


def id3junk(data):
    return id3_tag() + bytes(100) + data


def id3id3(data):
    return id3_tag() + id3_tag() + data


def nocounts(data):
    data = bytearray(data)
    if data[36:40] == b"VBRI":
        # id, version, delay, quality, then the byte and frame counts
        data[46:54] = bytes(8)
        return bytes(data)
    at = xing_offset(data)
    flags = struct.unpack(">I", data[at + 4:at + 8])[0]
    if (flags & 0x3) != 0x3:
        sys.exit("the Info frame lacks the frame or byte count")
    data[at + 8:at + 16] = bytes(8)
    return fix_crc(data)


def l399(data):
    data = bytearray(data)
    at = encoder_string_offset(data)
    data[at:at + 9] = b"L3.99r   "
    return fix_crc(data)


def fix_crc(data):
    """Recompute the LAME tag CRC (over the frame up to it)."""
    crc_at = encoder_string_offset(data) + 34
    struct.pack_into(">H", data, crc_at, lame_crc16(data[:crc_at]))
    return bytes(data)


VARIANTS = {"junk": junk, "id3junk": id3junk, "id3id3": id3id3, "nocounts": nocounts, "l399": l399}


def main():
    src, out, variant = sys.argv[1:4]
    with open(src, "rb") as f:
        data = f.read()
    with open(out, "wb") as f:
        f.write(VARIANTS[variant](data))


if __name__ == "__main__":
    main()
