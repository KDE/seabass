<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC0-1.0
-->

# Click MP3s (#50)

Three seconds of digital silence with two one-sample clicks, the same in
every channel:

| click        | time    | sample at 44.1 kHz | 48 kHz | 22.05 kHz |
|--------------|---------|--------------------|--------|-----------|
| full scale   | 1.500 s | 66150              | 72000  | 33075     |
| 0.25 of full | 2.500 s | 110250             | 120000 | 55125     |

One file per mp3guessenc class, so the same audio instant can be found in
each decoder's output and the lag between decoders measured:

| file                          | class | what it is                                         |
|-------------------------------|-------|----------------------------------------------------|
| click-A-notag-44100.mp3       | A     | libmp3lame CBR, no Info frame                      |
| click-B-noencoder-44100.mp3   | B     | the D file with its 9-byte encoder string zeroed   |
| click-B-lavc-44100.mp3        | B     | ffmpeg libmp3lame: "Lavc" Info frame, stereo over joint stereo |
| click-C-badcrc-44100.mp3      | C     | the D file with its LAME tag CRC broken            |
| click-D-lame-44100.mp3        | D     | libmp3lame CBR with Info frame and LAME tag        |
| click-D-lame-48000.mp3        | D     | the same at 48 kHz (80 kbit/s)                     |
| click-D-lame-mpeg2-22050.mp3  | D     | the same as MPEG-2 Layer III at 22.05 kHz          |
| click-D-lame-mono-44100.mp3   | D     | the same in mono                                   |
| click-VBRI-44100.mp3          | D     | the A file behind a crafted VBRI frame in the audio's channel mode |
| click-VBRI-modediffers-44100.mp3 | D  | the same with the VBRI frame stereo over joint stereo audio |
| click-D-lame-crc-44100.mp3    | D     | the 44.1 kHz file with CRC-protected frames; the tag still sits at 4 + side info |
| click-D-lame-junk-44100.mp3   | D     | the D file behind 100 zero bytes: FFmpeg plays the Info frame and trims nothing |
| click-D-lame-id3junk-44100.mp3 | D    | an empty ID3v2.3 tag, 100 zero bytes, then the D file: the same          |
| click-D-lame-id3id3-44100.mp3 | D     | two empty ID3v2.3 tags, then the D file: the Info frame still first      |
| click-D-lame-nocounts-44100.mp3 | D   | the D file with the Info frame's frame and byte counts 0: FFmpeg plays the frame and still trims |
| click-D-l399-44100.mp3        | D     | the D file with the encoder string "L3.99r": FFmpeg skips the Info frame and trims nothing |
| click-VBRI-nocounts-44100.mp3 | D     | the VBRI file with its byte and frame counts 0: FFmpeg plays the frame  |

All CBR, 64 kbit/s except 80 at 48 kHz (a 64 kbit/s frame there is too
short for libmp3lame to write the tag frame).

Regenerate with

    tools/mp3-offset-probe/make-click-mp3s.py

(the VBRI frames come from vbri_frame.py: no encoder at hand writes
VBRI; the moved and emptied tag frames from tag_frame_variants.py), which needs libmp3lame (through ctypes) and the ffmpeg command line. The
LAME files come out byte for byte the same each run; the Lavc file
carries the ffmpeg version in its tags.

`mp3_lead_in_dump` prints what the model makes of each file;
`mp3_decoder_offset_check` decodes them with libmpg123 and libavformat
and compares the measured lag with the model.
