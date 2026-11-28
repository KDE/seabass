<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC0-1.0
-->

# Two tones, one a channel

A second and a half of a 220 Hz sine on the left and a 587 Hz sine on the
right, in five formats. For `ffmpeg_pcm_source_test`, which holds the two
decoders to the same samples: the other audio fixtures are mono or have
the same audio on both channels, so a decoder that read only the left
channel, or mixed the two wrongly, would pass on them.

Made with FFmpeg 6.1.1:

    ffmpeg -f lavfi -i sine=frequency=220:duration=1.5 \
           -f lavfi -i sine=frequency=587:duration=1.5 \
           -filter_complex '[0][1]join=inputs=2:channel_layout=stereo[a]' -map '[a]' \
           -map_metadata -1 -fflags +bitexact <codec options> <file>

with `-c:a libmp3lame -b:a 96k`, `-c:a aac -b:a 96k`, `-c:a libvorbis -q:a 3`,
`-c:a flac -sample_fmt s32 -bits_per_raw_sample 24` and `-ar 11025 -c:a pcm_s16be`.

# A tone on the right only

`left-silent-right-tone.flac`: a second and a half of silence on the left;
on the right a 587 Hz sine from 0.25 s to 1.25 s. For `ffmpeg_decoders_test`:
the silence probe takes the loudest channel of every frame, so a probe that
looked at the left channel only would call this file silent throughout.

    ffmpeg -f lavfi -i anullsrc=r=44100:cl=mono:d=1.5 -f lavfi -i sine=frequency=587:duration=1 \
           -filter_complex '[1]adelay=250[t];[0][t]join=inputs=2:channel_layout=stereo[a]' -map '[a]' \
           -t 1.5 -map_metadata -1 -fflags +bitexact -c:a flac left-silent-right-tone.flac
