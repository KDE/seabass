#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Encode one second of silence with a click into an MP3 through libmp3lame.

The `lame` command line tool is a thin frontend over this same library;
it is not installed everywhere libmp3lame is (FFmpeg pulls the library
in), so make-lead-in-fixtures.sh drives the library directly through
ctypes and does what the frontend does: write the frames, then put the
finished Xing/Info + LAME tag frame from lame_get_lametag_frame() over
the placeholder frame the encoder emitted first.

usage: lame_encode.py OUT RATE CHANNELS MODE KBPS|vN [notag]
  MODE: stereo, joint, mono. vN asks for VBR at quality N.

Other scripts import encode() to put their own samples through the same
path (make-click-mp3s.py).
"""

import ctypes
import ctypes.util
import struct
import sys


def encode(out, rate, channels, mode, quality, notag=False, pcm=None):
    """Write PCM (16-bit ints, one per sample; every channel gets the
    same) to OUT. Without PCM: one second of silence with a full-scale
    click at half a second."""
    if pcm is None:
        pcm = [0] * rate
        pcm[rate // 2] = 32000
    count = len(pcm)

    name = ctypes.util.find_library("mp3lame") or "libmp3lame.so.0"
    lib = ctypes.CDLL(name)
    lib.lame_init.restype = ctypes.c_void_p
    lib.lame_get_lametag_frame.restype = ctypes.c_size_t
    lib.lame_get_lametag_frame.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
    for fn in ("lame_set_in_samplerate", "lame_set_out_samplerate", "lame_set_num_channels",
               "lame_set_mode", "lame_set_brate", "lame_set_VBR", "lame_set_VBR_q",
               "lame_set_bWriteVbrTag", "lame_init_params", "lame_close"):
        getattr(lib, fn).argtypes = [ctypes.c_void_p] + ([ctypes.c_int] if fn not in ("lame_init_params", "lame_close") else [])
    lib.lame_encode_buffer_interleaved.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int,
                                                   ctypes.c_char_p, ctypes.c_int]
    lib.lame_encode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int,
                                       ctypes.c_char_p, ctypes.c_int]
    lib.lame_encode_flush.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]

    gf = lib.lame_init()
    lib.lame_set_in_samplerate(gf, rate)
    lib.lame_set_out_samplerate(gf, rate)
    lib.lame_set_num_channels(gf, channels)
    lib.lame_set_mode(gf, {"stereo": 0, "joint": 1, "mono": 3}[mode])
    if quality.startswith("v"):
        lib.lame_set_VBR(gf, 4)  # vbr_mtrh, the frontend's -V
        lib.lame_set_VBR_q(gf, int(quality[1:]))
    else:
        lib.lame_set_VBR(gf, 0)
        lib.lame_set_brate(gf, int(quality))
    lib.lame_set_bWriteVbrTag(gf, 0 if notag else 1)
    if lib.lame_init_params(gf) < 0:
        sys.exit("lame_init_params failed")
    # No lame_init_bitstream() here: the library writes the placeholder
    # tag frame (InitVbrTag) itself before the first frame, and a second
    # call writes a second one, a silent frame the tag's delay does not
    # count, so every decoder put the audio 1152 samples late.

    samples = struct.pack("<%dh" % count, *pcm)
    buf = ctypes.create_string_buffer(count * 2 + 7200)
    if channels == 1:
        n = lib.lame_encode_buffer(gf, samples, samples, count, buf, len(buf))
    else:
        inter = struct.pack("<%dh" % (2 * count), *[v for v in pcm for _ in (0, 1)])
        n = lib.lame_encode_buffer_interleaved(gf, inter, count, buf, len(buf))
    if n < 0:
        sys.exit("encode failed: %d" % n)
    data = bytearray(buf.raw[:n])
    tail = ctypes.create_string_buffer(7200)
    m = lib.lame_encode_flush(gf, tail, len(tail))
    data += tail.raw[:m]
    if not notag:
        tag = ctypes.create_string_buffer(2880)
        size = lib.lame_get_lametag_frame(gf, tag, len(tag))
        if size == 0 or size > len(data):
            sys.exit("no lametag frame: %d of %d" % (size, len(data)))
        data[:size] = tag.raw[:size]
    lib.lame_close(gf)
    with open(out, "wb") as f:
        f.write(data)


def main():
    out, rate, channels, mode, quality = sys.argv[1:6]
    notag = len(sys.argv) > 6 and sys.argv[6] == "notag"
    encode(out, int(rate), int(channels), mode, quality, notag)


if __name__ == "__main__":
    main()
