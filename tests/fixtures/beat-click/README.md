<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC0-1.0
-->

# Beat-click MP3s, 10 seconds

The files of `tools/mp3-offset-probe/make-beat-click-mp3s.py`, cut to ten
seconds: a click every 500 ms from 1.000 s to 9.500 s, one file per
mp3guessenc class (A, B with the encoder string zeroed, B "Lavc", C, D at
44.1 and 48 kHz). The WAV the script also writes is left out;
`audio_detail_waveform_test` writes the same audio itself.

Regenerate with

    tools/mp3-offset-probe/make-beat-click-mp3s.py tests/fixtures/beat-click 10
    rm tests/fixtures/beat-click/beats-ref-44100.wav

The Lavc file carries the ffmpeg version in its tags.
