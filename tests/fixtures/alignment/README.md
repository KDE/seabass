<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC0-1.0
-->

# Copies of one song with a known shift

Pairs of mono files for `AudioAlignmentProbe::shiftMs(a, b)`: where a
sound sits in b minus where it sits in a, on the decoder's timeline, or
unknown. Every pair's right answer was established without the code
under test. `tests/alignment_fixture_test.cpp` holds the probe to it, and
`tests/alignment_cue_lands_test.cpp` checks, end to end, that a cue copied
or merged between two of these files lands on the sound it marked.

## How the truth is known

Each signal is made from functions of time, so it can be sampled at any
rate, and carries marker events: short bursts of six tones with a sharp
onset, each one different. `tools/make_alignment_fixtures.py` writes the
copies and then establishes the truth twice:

* by construction: where the source's time 0 sits in each file;
* by measurement: every file is decoded with the ffmpeg command line and
  every marker found in the decoded samples by a matched filter, to a
  fraction of a sample.

For a lossless file the two must agree to 0.01 ms, or the script stops.
For an encoded file the measurement is the truth: an encoder's delay and
priming, and whether ffmpeg trims them, are measured, not assumed (the
`codec_ms` column; class A MP3 is 25.057 ms late, the rest under 0.001 ms).
A pair's markers must agree with its truth to 0.1 ms.

`alignment_fixture_test` also decodes every file with the Qt decoder the
probe uses and checks, marker by marker, that its samples line up by the
amount ffmpeg's do (they agree to well under 0.001 ms on every file here).

## The manifests

`pairs.tsv`, one line per pair (the test asks both ways round, the truth
negated):

| column        | meaning |
|---------------|---------|
| id            | the pair's name |
| shard         | which `alignment_fixture_test_<shard>` runs it |
| a, b          | the files |
| shift_ms      | the true shift on the decoder's timeline, or `unknown` |
| also_accepted | other right answers, `\|`-separated (a shift or `unknown`), `-` for none |
| why           | what was done, and why the truth is what it is |

`files.tsv`, one line per file: rate, format, length, MP3 lead-in class
and `cueTimelineOffsetMs` as `mp3_lead_in_dump` reads them (0 for any
other format), the measured codec delay, and every marker's time on the
decoder's timeline (`id:ms`).

## The files

Sources, all mono:

* `a`: eight tone bursts at irregular times in 4.5 s of digital silence;
  `o` (in `a-other`) and `t` (the extra tail) are the same kind with
  other seeds.
* `b`: a drum loop at 120 BPM (beat 500 ms, bar 2000 ms) that repeats
  exactly, sample for sample, bar after bar. Kick on every beat, hats,
  claps on 2 and 4, and a stab on the one, which makes the bar and not
  the beat its period. 8 kHz.
* `n`: `b` with 1% of its energy in slow tones that do not repeat.
* `c`: the loop with every bar a little different (levels, ghost hats),
  a quiet pad that does not repeat, and seven tone bursts at irregular
  times (the markers). 8 kHz.
* `d`: three seconds of digital silence, then 4.5 s of slow swelling
  tones with five bursts in them. 11.025 kHz.
* `L`: forty bursts over 85 s, so the probe's start, middle and end
  windows (40 s each) are different stretches. 11.025 kHz.
* `j`: a DJ track, 135 s at 4 kHz. 45 s of a beat-only loop (kick and
  stab, every hit's level varied by up to 3% a bar), 45 s of tone bursts
  at irregular times with silence between (nothing in it repeats), and
  45 s of the loop again. The probe's start and end windows are all loop:
  a bar away scores 0.99 of the true lag there. Its middle window has no
  rival. One marker in the intro and one in the outro, among the loop.
* `k`: the same loop for all 135 s, with one marker in it.

| file | what it is |
|------|------------|
| a-ref.flac | `a` at 44.1 kHz |
| a-lead21.flac, a-lead46.flac, a-lead675.flac, a-lead1300.flac | 926, 2029, 29768 and 57330 samples of silence in front (21, 46, 675 and 1300 ms) |
| a-trim300.flac | the first 300 ms taken off |
| a-tail.flac | 2 s of other bursts added at the end |
| a-gain6.flac | 6 dB quieter |
| a-invert.flac | polarity inverted |
| a-other.flac | another signal of the same length |
| a-cut.flac | 400 ms cut out at 1.6 s |
| a-48k-lead120.flac | `a` sampled at 48 kHz, 120 ms of silence in front |
| a-ref.wav, a-lead160.wav | `a` at 11.025 kHz, and with 160 ms in front |
| a-ref-B.mp3 | ffmpeg's libmp3lame, 48 kbit/s: a "Lavc" Info frame, class B |
| a-lead46-A.mp3 | libmp3lame without an Info frame, 64 kbit/s, 46 ms in front: class A |
| a-lead21-D.mp3 | libmp3lame with a LAME tag and a valid CRC, 64 kbit/s, 21 ms in front: class D |
| a-ref.m4a, a-lead160.m4a | ffmpeg's AAC in MP4, 48 kbit/s, and with 160 ms in front |
| b-ref.flac, b-lead730.flac | the exact loop, and with 730 ms of silence in front |
| b-bar.flac | the exact loop with one more bar of it in front |
| n-ref.flac, n-eq-1300.flac | two windows of the same length of `n`, the second 1.3 s further in |
| c-ref.flac, c-lead620.flac, c-lead1300.flac | the varied loop, and with 620 ms (more than a beat) and 1.3 s (more than half a bar) in front |
| c-eq-1300.flac | a window of the varied loop as long as c-ref, 1.3 s further in |
| d-ref.flac, d-intro675.flac, d-intro675.m4a | the music, and with the intro 675 ms longer (as FLAC and as AAC) |
| L-ref.flac, L-tail.flac, L-cut.flac | the long one, with 6 s of other bursts at the end, and with 400 ms cut out at 42.5 s |
| j-ref.flac, j-lead21.flac, j-lead1300.flac | the DJ track, and with 21 ms and 1.3 s (more than a beat) in front |
| j-barcut.flac | the DJ track with a whole bar (2 s) cut out of its middle at 66 s |
| j-insert.flac | the DJ track with the bar before 22 s played twice, in its loop intro |
| k-ref.flac, k-lead21.flac | the loop all through, and with 21 ms in front |

The answers that are not one number, and why:

* `invert`: every sound is where it was, so 0 is right. A probe that
  looks for positive correlation only may say unknown, which moves no
  cue; that is accepted. Any other value is not.
* `cut-short`, `long-cut`: sounds before the cut are 0 apart, after it
  400 ms. No one shift is right for every cue: unknown.
* `dj-21`, `dj-1300`: known. The start and end windows each have a rival
  a bar away at 0.99 of the true lag; the middle window has none, and
  every window's own best is the true lag. Windows of their own settle
  each other that way (`ResolvingRivalShare`), the way the copies of a
  real track with a beat-only intro and outro need.
* `dj-barcut`: the start says 0, the end a bar early. Unknown.
* `dj-loop`: every window has a rival a bar away at 0.99 or more; none
  settles the others. Unknown.
* `dj-insert`: a bar played twice inside the loop intro of one copy. The
  middle and end windows say +2000 ms, which is right for every sound
  from 22 s on. Before 22 s the copies are 0 apart, and inside an exact
  loop the audio cannot tell where the extra bar went: the bars on either
  side of the insertion are the same bars, so "a bar inserted at 22 s"
  and "a bar inserted at 6 s" make the same file. The start window's own
  best is 0 (+2000 scores 0.9998 of it). The rule takes a shift only when
  every window's own best agrees, so this is unknown, which is accepted.
  Had the start window's best been +2000, "known, +2000" would have been
  accepted as well: what the audio cannot decide, a cue in the loop cannot
  be wrong about by more than the loop itself repeats.
* `other`: no shift lines the two up: unknown.
* `pure-730`: the loop repeats exactly; only the silence in front tells
  the true 730 ms from 730 ms plus or minus a bar. The true value or
  unknown.
* `pure-bar`: b-bar is b-ref sample for sample from 0 s, and again from
  2 s. 2000 ms is how it was made and 0 puts every cue on the same
  samples: both are right, and unknown is accepted.
* `near-eq-1300`: the review's case. -1300 ms is true and is in the
  signal (the 1% that does not repeat), but a bar either side correlates
  at about 0.99 of it, so unknown is accepted too.
* `real-eq-1300`: the realistic loop. The variation and the bursts make
  -1300 ms the only right answer.

## Regenerating

    tools/make_alignment_fixtures.py <build>/mp3_lead_in_dump

Needs python3 with numpy, the ffmpeg and ffprobe command lines (with
libmp3lame and the native AAC encoder) and libmp3lame itself, which
`tools/mp3-offset-probe/lame_encode.py` drives through ctypes for the
class A and D files. Every random choice is seeded, and ffmpeg is told to
write no version or date, so a rerun writes the same bytes (checked with
ffmpeg 6.1.1 and libmp3lame 3.100). The directory weighs about 1.5 MB.
