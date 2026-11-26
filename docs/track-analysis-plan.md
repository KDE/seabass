<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC0-1.0
-->

# Track analysis: plan

Seabass computes the beat grid, the waveforms and the key of a track from
its audio, so a library needs no rekordbox to be playable. This file is
the working plan; it changes as phases land.

## Rules that hold for every phase

- **A grid is a list of beats.** The analyser gives `std::vector<domain::Beat>`
  (a time per beat, a place in the bar), the shape rekordbox stores and
  `markersFromBeats()` turns into Engine markers. A constant tempo is a
  fit through that list. Grids whose tempo moves (drift, ramps, steps, a
  live drummer) are first-class from the first commit: every metric and
  every fixture set has them.
- **No whole-number BPM rule.** The tempo is written as measured.
- **The decoder timeline.** Sample 0 of a `PcmSource` is the first sample
  Qt Multimedia gives, which is Engine's timeline. A rekordbox grid is put
  on it with the existing lead-in code before anything is compared.
- **An existing grid is never replaced unasked.**
- **Analysers are domain code over the `PcmSource` port**, so their tests
  run on generated samples and link no decoder.

## Fixtures, three tiers

| Tier | What | Where |
|---|---|---|
| 1 | Generated music-like tracks with a known grid (`seabass_synth_track`), made when the test runs; a few short encoded files where a codec is the point | in the repository |
| 2 | Real analysis data without audio: rekordbox grids and every waveform section, Engine grids and overviews, de-identified | a zip on the website, fetched on request |
| 3 | Real audio | private, on the developer's machines only; never on a server |

Tier 1 covers every code path, so KDE's CI, which does not fetch tier 2,
loses accuracy evidence and no coverage.

Tier 2: `-DSEABASS_FETCH_FIXTURES=ON` downloads
`https://vizzzion.org/seabass/downloads/fixtures/<name>-<date>.zip` at
configure time into the build tree, checked against a SHA-256 pinned in
`cmake/SeabassFixtures.cmake`; `-DSEABASS_REAL_ANALYSIS_FIXTURE=<dir>`
points at an unpacked copy instead. A test that wants the data skips,
naming both switches, when neither is given, and fails when a directory
is given and its `MANIFEST.json` is missing or does not match.

Tier 3: `SEABASS_PRIVATE_AUDIO=<dir>`, for `tools/analysis_benchmark` only.
No test in ctest depends on it.

## Phases

1. **Benchmark and PCM source.** `domain::PcmSource`, its Qt Multimedia
   implementation, the synthetic track generator, the grid comparison
   (`domain/beat_grid_comparison.hpp`), `tools/analysis_benchmark`.
   Done when the tool reproduces the known rekordbox to Engine residuals
   on the private library copy, and a grid shifted on purpose is reported
   as exactly that shift, on constant and on moving grids.
2. **Baseline.** The method Mixxx uses (qm-dsp, its ironing) wired into
   the benchmark tool only. Numbers decide whether qm-dsp ships.
3. **Seabass's grid analyser.** Tempo curve, sample-level phase, downbeat,
   confidence. A constant fit where the track is constant, a moving grid
   where it is not.
4. **Waveforms from audio.** rekordbox's sections and Engine's overview.
5. **Key.**
6. **Writers.** Complete ANLZ files; Engine through the existing writer.
7. **Review and correction page.**
8. **Players.** One capture round.

## The grid comparison

Per pair of grids (candidate, reference), after a shift the caller gives:

- **offset**: each reference beat against the nearest candidate beat, in
  ms: the median (the systematic shift), the median absolute deviation
  around it (the scatter), the 95th percentile and the maximum of the
  absolute value;
- **hit rate** at 1, 3, 8 and 20 ms;
- **drift**: how far the candidate runs away from the reference from
  the start of the track to its end, in ms: what a tempo that is a
  little off adds up to (0.01 BPM at 128 is 28 ms after six minutes, on
  a grid whose first beats are exact). The offset is followed from beat
  to beat in stretches. A hole in the candidate, a stray beat and a
  jump of more than a quarter of a beat (a while on the off-beat) are
  not drift; the slope of the longest stretch is taken over the whole
  track. Said only at the same tempo, and when one stretch has half of
  the beats followed;
- **tempo relation**: same, half, double, two thirds, three halves, other,
  from the ratio of the local beat periods, so it holds on a moving grid;
- **bar**: by how many beats the candidate's bar is turned against the
  reference's (0 to 3), and on what share of the matched beats;
- **shape** of each grid: constant (a line through index and time leaves
  no residual above 1.5 ms; the BPM is its slope) or moving, with the
  number of straight runs and the lowest and highest local BPM;
- **cover**: the share of the reference's span the candidate has beats for.

rekordbox stores whole milliseconds; a difference under 1 ms means nothing.

## The synthetic tracks

`seabass_synth_track` (a static library for tools and tests, not shipped)
renders mono float PCM and the true grid from a `SynthTrackSpec`:

- instruments: kick, snare or clap on 2 and 4, hats, a bass line and pad
  chords in a stated key, reverb, the pad and bass ducked under the kick;
- arrangement: sections that switch instruments on and off (a drumless
  intro, a breakdown without kick, a fill);
- tempo programme: constant (whole or fractional BPM), slow drift, ramp,
  step, alternating runs, and a live drummer (hits scatter around the
  true beat; the true grid stays the smooth one);
- swing on the off-beats, a first beat that is not at sample 0, a rate of
  44.1 or 48 kHz;
- its own random generator and distributions, so one seed gives the same
  track on every platform and standard library.

Named presets cover the list above. `synth_track --preset NAME --out
x.wav --grid x.tsv` writes one for listening or for an encoder.

## Phase 2 results

Mixxx 2.5.3's method (qm-dsp from Mixxx's tree, its configuration and its
ironing ported in `tools/analysis_baseline/`), measured with
`analysis_benchmark`. Offsets are the analyser's beat minus the
reference's, in ms. `qm-raw` is the tracker's beat list as it comes,
`mixxx-constant` Mixxx's default constant grid with its BPM rounding,
`mixxx-constant-unrounded` the same without the rounding.

### Generated tracks

Against the true grid. median and scatter: the offset's median and its
median absolute deviation; then the share of true beats within 1, 3, 8
and 20 ms; the tempo relation; the found grid's shape; the mean tempo,
first beat to last, of the truth and of the found grid; the drift in ms.
The tracks are 77 to 152 seconds long.

| preset | analyser | median | scatter | 1 ms | 3 ms | 8 ms | 20 ms | tempo | shape | true BPM | found BPM | drift |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| house-124 | qm-raw | -3.7 | 3.7 | 0.11 | 0.31 | 0.68 | 0.86 | same | moving | 124.000 | 124.483 | -600.2 |
| house-124 | mixxx-constant | -3.6 | 0.0 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 124.000 | 124.000 | 0.0 |
| house-124 | mixxx-constant-unrounded | -3.6 | 1.0 | 0.00 | 0.35 | 1.00 | 1.00 | same | constant | 124.000 | 124.004 | -3.8 |
| trance-137.98 | qm-raw | +0.1 | 210.4 | 0.06 | 0.16 | 0.35 | 0.43 | same | moving | 137.980 | 138.330 | -273.8 |
| trance-137.98 | mixxx-constant | +210.4 | 3.5 | 0.00 | 0.00 | 0.00 | 0.00 | same | constant | 137.980 | 138.000 | -14.0 |
| trance-137.98 | mixxx-constant-unrounded | +216.1 | 0.6 | 0.00 | 0.00 | 0.00 | 0.00 | same | constant | 137.980 | 137.977 | +2.3 |
| drift-122 | qm-raw | -4.3 | 3.8 | 0.11 | 0.31 | 0.65 | 0.79 | same | moving | 122.047 | 122.006 | +0.5 |
| drift-122 | mixxx-constant | -71.4 | 66.3 | 0.04 | 0.11 | 0.20 | 0.30 | same | constant | 122.047 | 121.667 | +275.1 |
| drift-122 | mixxx-constant-unrounded | -24.6 | 113.3 | 0.01 | 0.12 | 0.19 | 0.25 | same | constant | 122.047 | 121.561 | +371.0 |
| ramp-100-125 | qm-raw | -4.2 | 3.9 | 0.09 | 0.25 | 0.66 | 0.85 | same | moving | 115.216 | 115.887 | -600.8 |
| ramp-100-125 | mixxx-constant | -7.3 | 41.7 | 0.01 | 0.02 | 0.40 | 0.42 | other | constant | 115.216 | 125.000 | - |
| ramp-100-125 | mixxx-constant-unrounded | -6.9 | 40.4 | 0.01 | 0.01 | 0.40 | 0.42 | other | constant | 115.216 | 124.994 | - |
| step-120-126 | qm-raw | -3.3 | 4.0 | 0.14 | 0.38 | 0.64 | 0.86 | same | moving | 124.218 | 124.708 | -594.8 |
| step-120-126 | mixxx-constant | -6.2 | 71.4 | 0.00 | 0.00 | 0.32 | 0.35 | other | constant | 124.218 | 120.000 | - |
| step-120-126 | mixxx-constant-unrounded | -6.1 | 72.8 | 0.00 | 0.00 | 0.32 | 0.34 | other | constant | 124.218 | 119.996 | - |
| alternating-127.5-128 | qm-raw | -2.3 | 227.6 | 0.03 | 0.12 | 0.31 | 0.43 | same | moving | 127.715 | 128.048 | -297.2 |
| alternating-127.5-128 | mixxx-constant | -200.8 | 19.2 | 0.00 | 0.00 | 0.00 | 0.00 | same | constant | 127.715 | 127.667 | +74.8 |
| alternating-127.5-128 | mixxx-constant-unrounded | -206.4 | 17.2 | 0.00 | 0.00 | 0.00 | 0.00 | same | constant | 127.715 | 127.684 | +61.1 |
| live-drummer-96 | qm-raw | -1.8 | 4.4 | 0.12 | 0.39 | 0.74 | 0.99 | same | moving | 96.151 | 96.173 | +1.7 |
| live-drummer-96 | mixxx-constant | +1.1 | 101.7 | 0.03 | 0.09 | 0.23 | 0.32 | same | constant | 96.151 | 95.333 | +834.8 |
| live-drummer-96 | mixxx-constant-unrounded | +0.2 | 99.7 | 0.03 | 0.12 | 0.23 | 0.32 | same | constant | 96.151 | 95.316 | +855.2 |
| breakbeat-swing-132 | qm-raw | -1.2 | 3.8 | 0.13 | 0.38 | 0.75 | 0.81 | same | moving | 132.000 | 131.705 | +280.3 |
| breakbeat-swing-132 | mixxx-constant | -2.3 | 0.0 | 0.00 | 1.00 | 1.00 | 1.00 | same | constant | 132.000 | 132.000 | 0.0 |
| breakbeat-swing-132 | mixxx-constant-unrounded | -2.2 | 0.3 | 0.00 | 1.00 | 1.00 | 1.00 | same | constant | 132.000 | 131.999 | +1.0 |
| long-breakdown-126 | qm-raw | -3.4 | 6.2 | 0.08 | 0.21 | 0.48 | 0.60 | same | moving | 126.000 | 126.315 | -587.7 |
| long-breakdown-126 | mixxx-constant | -3.9 | 0.0 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 126.000 | 126.000 | 0.0 |
| long-breakdown-126 | mixxx-constant-unrounded | -3.9 | 0.1 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 126.000 | 126.000 | +0.5 |
| drumless-intro-128 | qm-raw | -3.7 | 8.6 | 0.07 | 0.17 | 0.39 | 0.53 | same | moving | 128.000 | 125.986 | +289.8 |
| drumless-intro-128 | mixxx-constant | -5.3 | 0.0 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 128.000 | 128.000 | 0.0 |
| drumless-intro-128 | mixxx-constant-unrounded | -2.4 | 3.5 | 0.15 | 0.43 | 0.90 | 1.00 | same | constant | 128.000 | 128.020 | -14.0 |
| house-124-48k | qm-raw | -3.2 | 3.5 | 0.14 | 0.33 | 0.72 | 0.85 | same | moving | 124.000 | 124.479 | -600.2 |
| house-124-48k | mixxx-constant | -3.3 | 0.0 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 124.000 | 124.000 | 0.0 |
| house-124-48k | mixxx-constant-unrounded | -3.3 | 0.1 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 124.000 | 124.000 | +0.4 |
| halftime-174 | qm-raw | +1.3 | 172.4 | 0.07 | 0.20 | 0.43 | 0.50 | half | moving | 174.000 | 87.018 | - |
| halftime-174 | mixxx-constant | +341.7 | 0.0 | 0.00 | 0.00 | 0.50 | 0.50 | half | constant | 174.000 | 87.000 | - |
| halftime-174 | mixxx-constant-unrounded | +0.1 | 172.4 | 0.10 | 0.25 | 0.50 | 0.50 | half | constant | 174.000 | 87.011 | - |

What the table says:

- Where the track is constant and the kick carries the beat (house,
  breakbeat, long breakdown, drumless intro, 48 kHz), the constant grid
  has the right tempo, rounded to the whole BPM, and sits 2 to 5 ms
  early: inside the tracker's 11.6 ms step, never under 1 ms.
- The raw tracker keeps the tempo but loses the phase for whole
  sections: it slides onto the off-beat in a breakdown and back (house-124:
  beats 96 to 125 are up to half a beat out), so 14 to 57 percent of its
  beats are more than 20 ms out even on a constant track.
- When the off-beat wins the longest constant region, the constant grid
  is half a beat out for the whole track (trance-137.98 at +210 ms,
  alternating-127.5-128 at -201 ms) while reporting the same tempo.
- A moving tempo (drift, ramp, step, a live drummer) is not followable
  with one constant grid by construction: the longest region's tempo is
  taken (125 for the ramp, 120 for the step).
- Half-time drum and bass is read at half the tempo (87 BPM) by all
  three.
- The rounding moves the 137.98 BPM track onto 138.000, a whole-number
  answer for a fractional track: 14 ms of drift in 97 seconds, where
  the unrounded 137.977 drifts 2.
- The raw tracker has more beats than the track: 228 for the 224 of
  house-124. Before the breakdown it runs fast for five beats (441 ms
  apart for 484), which puts it on the off-beat; after it, it runs fast
  for five more and is on the beat again, one beat ahead. Its drift of
  about 600 ms there is that beat (a step in the offset is read a
  quarter large), on ramp-100-125, step-120-126 and long-breakdown-126
  too; about half a beat on four more presets. Only on the two tracks
  with no breakdown and a tempo that moves slowly (drift-122,
  live-drummer-96) does it keep count.

Key (`qm-key` against the preset's stated key): right on the two major
presets ramp-100-125 (D) and drumless-intro-128 (G); every minor preset
comes back as its relative major (A minor as C, C minor as E flat,
G minor as B flat, F minor as A flat, D minor as F), and E major as its
relative C sharp minor.

### Real music

rekordbox's grids are the reference here, and they are its automatic
analysis, not corrected by hand: a difference can be rekordbox's error.
Truth exists only for the generated tracks. The audio is private (tier
3); what is written here is numbers.

#### The collection: 1475 tracks

A rekordbox collection of mostly house and techno, analysed by
rekordbox 7.2.18 in its normal mode: 1312 MP3 and 163 M4A files, every
rekordbox grid constant, 90.4 percent of them at a whole-number tempo
(within 0.005 BPM). `analysis_benchmark audio <track list> --analyser all
--half all --jobs 14`, 13 minutes. Both halves: these are the baseline's
figures and nothing was tuned on them.

One outcome a track, the first that holds:

| outcome | qm-raw | mixxx-constant | mixxx-constant-unrounded |
|---|---|---|---|
| another tempo (half, double, two thirds, other) | 25 (1.7 %) | 29 (2.0 %) | 29 (2.0 %) |
| the same tempo, half a beat out | 299 (20.3 %) | 606 (41.1 %) | 608 (41.2 %) |
| on the beat, more than 10 ms from rekordbox | 137 (9.3 %) | 68 (4.6 %) | 64 (4.3 %) |
| within 10 ms, drifts more than 10 ms | 721 (48.9 %) | 40 (2.7 %) | 183 (12.4 %) |
| within 3 ms, no such drift | 137 (9.3 %) | 351 (23.8 %) | 267 (18.1 %) |
| 3 to 10 ms, no such drift | 156 (10.6 %) | 381 (25.8 %) | 324 (22.0 %) |

"Half a beat out" is a median offset of more than three tenths of a
beat. The three things scored apart:

- **Spread between tracks** (mixxx-constant, the 840 tracks on the
  beat): the median offset is 3.0 ms early, half of the tracks between
  4.3 and 1.6 ms early, nine in ten between 13.4 ms early and 3.9 ms
  late. 87 percent are early. Late by more than 3 ms: 47 tracks (3.2
  percent of all), by more than 10 ms: 21.
- **Drift over the track** (the 1446 tracks at rekordbox's tempo):
  with the rounding the median is 0.6 ms, within 3 ms on 88.7 percent,
  within 10 ms on 92.5; without it the median is 5.6 ms, within 3 ms on
  29.1 percent, within 10 ms on 70.5. The raw tracker: median 313 ms,
  within 10 ms on 29.3 percent.
- **Gross errors**: the first two rows of the table.

The tempo itself, against rekordbox's, on those 1446 tracks:

| within | mixxx-constant | mixxx-constant-unrounded |
|---|---|---|
| 0.001 BPM | 79.0 % | 13.2 % |
| 0.005 BPM | 92.9 % | 77.4 % |
| 0.01 BPM | 94.7 % | 89.5 % |
| 0.05 BPM | 97.9 % | 98.0 % |

A track of the median length (334 seconds) at 128 BPM drifts 10 ms at
0.004 BPM. On the 130 of them whose rekordbox tempo is not a whole
number, the drift stays within 10 ms on 37.7 percent with the rounding
and on 41.5 without.

Key (`qm-key` against rekordbox's): the same on 53.6 percent; the
parallel key on 9.4, a fifth down on 8.2, the relative key on 4.9, a
fifth up on 3.7, something else on 20.1.

What it says:

- The phase is the main error, not the tempo: two tracks in five are
  half a beat from rekordbox with a constant grid, one in five with the
  raw beats. Who is right is not known yet; the next step measures the
  audio at both grids on every one of them.
- Where the phase agrees the grid is early by 3 ms, and tightly so. A
  constant brings the median to rekordbox's; early is the safe side (a
  late grid cuts a quantised loop into the kick).
- The method does not measure the tempo well enough to leave it
  unrounded: 0.002 BPM off at the median, 0.011 at the ninth decile,
  against the 0.004 that 10 ms needs. Rounding to the whole number
  repairs that on this collection, nine tenths of which is at a whole
  number, and helps nothing on the tracks that are not. The rule above
  (no whole-number rule) stands as decided; what the numbers ask for is
  a tempo measured over the whole track to a few thousandths, and
  rounding only where the measurement cannot tell the two apart.
- The raw tracker loses count: of the 1443 tracks it covers from end
  to end, 271 have rekordbox's number of beats. Its drift is beats
  gained or lost on the way, as on the generated tracks, not a tempo
  that is off. This tracker's beat list is not usable as a grid without
  a fit on top.

#### Who is right, half a beat apart

On the 606 tracks where mixxx-constant has rekordbox's tempo and sits
half a beat from it, the audio was asked where the kick is. The band
from 30 to 120 Hz, its envelope, the rises of the envelope; these folded
onto one beat of rekordbox's grid, 32 beats at a time. Each block of 32
with bass in it votes for the place in the beat where its rises peak:
within an eighth of a beat of rekordbox's beat, within an eighth of
ours, or neither. Two thirds of the blocks decide a track.

What the measure is worth was looked at first:

- On the twelve generated tracks, the true grid against a grid half a
  beat from it: right on eleven. Wrong on breakbeat-swing-132, where the
  kick is not on every beat and the low band peaks between the beats:
  the measure holds for a kick on every beat and for nothing else.
- On the 772 tracks of the collection where the two grids agree within
  10 ms, rekordbox's beat against the place half a beat from it:
  rekordbox's beat on 586 (75.9 percent), unclear on 137 (17.7), half a
  beat away on 49 (6.3). So about one answer in sixteen goes against
  two analysers that agree.

On the 606:

| the kick is | tracks |
|---|---|
| on rekordbox's beat | 411 (67.8 %) |
| unclear | 138 (22.8 %) |
| on ours | 57 (9.4 %) |

The 9.4 percent for our grid is little more than what the measure gets
wrong on its own (6.3). On the tracks half a beat apart rekordbox has
the kick and the method has the place between: the phase is picked
wrong, on two tracks in five of this collection. That is the first
thing the analyser has to do better, and the low band is where to look.

The worklist (every track that differs, by kind, with this verdict, by
row id) is kept beside the private audio, not here.

#### The variety list: 110 tracks

114 files in 21 groups picked for what breaks an analyser: live
drummers, breakbeat, garage, drum and bass, half-time, hip-hop, reggae,
odd metres, tempo changes, no drums, jazz, very fast, very slow, long
breakdowns. 98 FLAC at 44.1 kHz, 7 second copies at 48 kHz, 9 MP3. Four
are the wrong recording and are left out. rekordbox 7.2.18 analysed
them twice: in its normal mode (one constant tempo a track; 105 of the
files, one without a grid) and in its dynamic mode (all of them, tempo
between 70 and 180 BPM).

rekordbox against itself, on the 101 files that have both grids:

- another tempo on 19, ten of them half or double;
- of the other 82, half a beat apart on 7;
- the dynamic grid is 5 ms earlier than the normal one at the median,
  and nine beats in ten are within 10 ms of each other on 3 tracks;
- in dynamic mode no grid is a line within 1.5 ms, the drum machines'
  included: every one of them is a moving grid by the comparison's test.

So on this music rekordbox is no reference to 3 or to 10 ms: its own
two answers are further apart than that. What it can give is the tempo
and the side of the beat where both answers agree, on 75 tracks.

The baseline against each, one outcome a track:

| outcome | mixxx-constant, normal | mixxx-constant, dynamic | qm-raw, normal | qm-raw, dynamic |
|---|---|---|---|---|
| tracks | 104 | 110 | 104 | 110 |
| another tempo | 24 | 22 | 26 | 20 |
| the same tempo, half a beat out | 8 | 6 | 7 | 8 |
| on the beat, more than 10 ms off | 27 | 22 | 27 | 8 |
| within 10 ms, drifts more than 10 ms | 26 | 35 | 28 | 25 |
| within 10 ms, no such drift | 19 (18.3 %) | 25 (22.7 %) | 16 (15.4 %) | 49 (44.5 %) |

The key is rekordbox's on 43.3 and 46.4 percent. The collection
flatters the method (49.6 percent within 10 ms and steady): it is the
music the method was made for. The raw beats are nearer the dynamic
grid than the constant grid is, which says the two follow the same
onsets, not that either is right.

## What the literature says

Read for the two errors that would not yield to tuning: a tempo taken
for its half or double, and the beat taken for the beat between.

- **A tempo or its double is not in the audio alone.** Percival and
  Tzanetakis (2014) score tempo candidates with pulse trains much as
  the analyser folds the track, and find their raw answer right on 52
  percent of 4011 files, half the true tempo on 33 and double on 5. An
  oracle that knew when to double would reach 85. Their best decider is
  a threshold (double anything under 71.9 BPM, 78 percent right); a
  support vector machine on three features of the tempo histogram does
  no better (76). Schreiber and Mueller (2017) treat it as
  classification after the fact, with a random forest on a
  "log beat spectrum" in five frequency bands plus spectral and
  temporal flatness, trained on a separate collection; it lifts their
  own estimator from 69 to 77 percent. Both need labelled tracks by the
  thousand, and the classifier is specific to the estimator it corrects.
- **DJ software settles it with a setting.** Gaertner (2013) compared
  Traktor, Serato Scratch Live, Cross, Torq and Virtual DJ with
  academic trackers on 2324 club tracks. Cross and Scratch Live offer
  only tempo ranges of exactly one octave (75 to 150, 68 to 135), "a
  simple but working approach"; Traktor offers nine such and one wide
  range, and restricted to 68 to 135 it had the tempo right on 90
  percent of the tracks against 78 when allowed 60 to 200, "which
  shows that automatically picking the right tempo octave is still an
  open issue". rekordbox has the same
  setting; the references here were made with 70 to 180, which is more
  than an octave and leaves 70 to 90 against 140 to 180 to rekordbox's
  own judgement. That is where the analyser disagrees with it.
- **Which moment is the beat depends on the genre.** Laroche (2003)
  measured the energy at the beat, the half beat and the quarter beats
  on tracks with known grids: the beat collects the most everywhere
  except in reggae, whose half beat is "often stronger than the
  downbeat" (snare and guitar chords), and Latin music, whose third
  quarter beat is. He weights a template 1 on the beat, 0.65 on the
  half beat, 0.2 on the quarters. Hockman, Davies and Fujinaga (2012),
  for jungle and drum and bass, sum the five lowest of 40 bands of an
  onset function because "drum types found at downbeats are likely to
  be kick drums": the same reasoning as leaving out the band above
  5 kHz here.
- **The state of the art is a neural network at 20 ms.** "Beat This!"
  (Foscarin, Schlueter, Widmer, 2024; code and weights under the MIT
  licence) reaches 89 percent beat F1 on GTZAN with 2 or 20 million
  parameters and gives a value every 20 ms, picked to the frame. That
  is seven times coarser than the 3 ms this plan counts as matching
  rekordbox, so it cannot place a grid, but it can say which side of
  the beat and which octave: a third opinion for the tracks on which
  the analyser and rekordbox differ. A 2026 failure analysis of such
  trackers ("The SMC Blind Spot") finds them "confident but wrong" on
  expressive music, with octave errors on 8 percent of a hard set.
- **Small timing errors are a matter of sampling.** Laroche again: the
  beat slightly off "can be minimized to an arbitrary level if the
  energy flux signal is sampled fast enough"; the tempo and the
  subdivision are the errors that matter. That matches what was found
  here: the offset is a calibration, the gross errors are the work.

What follows for the analyser: a tempo range as a setting, as
rekordbox and every DJ program has it, since the audio will not say;
a classifier for the octave only with far more labelled varied music
than the 54 tuning tracks there are; and a neural tracker run beside
the benchmark as a third opinion, not shipped.

Sources: Percival and Tzanetakis, "Streamlined Tempo Estimation Based
on Autocorrelation and Cross-correlation With Pulses", IEEE/ACM TASLP
22(12), 2014. Schreiber and Mueller, "A Post-Processing Procedure for
Improving Music Tempo Estimates Using Supervised Learning", ISMIR 2017.
Gaertner, "Tempo Detection of Urban Music Using Tatum Grid Non-Negative
Matrix Factorization", ISMIR 2013. Laroche, "Efficient Tempo and Beat
Tracking in Audio Recordings", JAES 51(4), 2003. Hockman, Davies and
Fujinaga, "One in the Jungle: Downbeat Detection in Hardcore, Jungle,
and Drum and Bass", ISMIR 2012. Foscarin, Schlueter and Widmer, "Beat
This! Accurate Beat Tracking Without DBN Postprocessing", ISMIR 2024,
arXiv 2407.21658. Ahn, Hwang and Jung, "The SMC Blind Spot", arXiv
2605.12287, 2026.

## The tempo range

The analyser takes a tempo range (`TempoRange` in
`lib/musicanalyze`, `grid.hpp`; `--tempo-range LOW-HIGH` in the
benchmark), as rekordbox does with its "BPM range". A tempo found
outside it is taken as its double or half inside it. The default is
rekordbox's default, 70 to 180, with which every reference grid here was
made. The setting has no place in the app yet: the analyser is not
wired into it. When it is, the ranges to offer are rekordbox's: 45-90,
58-115, 68-135, 70-180, 78-155, 88-175, 98-195.

- A range narrower than an octave is widened at the top to one
  (rekordbox's own are a hair under: 88 to 175), and both ends have half
  a percent of room, so that a track at 70.00 BPM is at 70 in a range
  from 70. Ranges are kept between 40 and 240 BPM. The benchmark prints
  the range a run was made with.
- Where the range has room for a tempo and its double, the one nearer
  128 BPM is preferred, as before. Not the one nearer the middle of the
  range: with the preference around 112 BPM (the middle of 70 to 180),
  29 tracks of the tuning half of the collection came out at another
  tempo than rekordbox's for 7, and 40 of the 110 of varied music for 29.
- In a range of one octave nothing is preferred. On real music that is
  a draw at 88 to 175 (below); in a range far from 128, such as 45 to
  90, a preference for 128 would take 90 over 60 on nearness alone.

What a range of one octave is worth, measured by running both sets with
88 to 175 and counting a tempo as right when it is rekordbox's brought
into the same range by octaves. That is what rekordbox would presumably
give with that setting; it was not run with it.

| | 70 to 180 (default) | 88 to 175 |
|---|---|---|
| Tuning half of the collection, 751 tracks: tempo right | 744 | 749 |
| Varied music, 110 tracks: tempo right | 81 | 91 |

With 128 preferred inside 88 to 175 as well: 747 and 93, two fewer and
two more.

A weakness seen while writing the tests, not yet measured on music:
every band counts alike in the onset function, however quiet, so a kick
that lives in one band with a faint figure every beat and a half in
three others comes out at two thirds of its tempo. Real kicks are in
every band, and of 751 collection tracks one comes out at two thirds;
of the 110 varied ones, four.

## Against Engine DJ

Engine DJ Desktop analysed the same 114 files of varied music; its
grids were read out of a copy of its database and scored with
`analysis_benchmark grids`. It gave every one of them a single straight
grid (two markers). Against rekordbox's Dynamic grids, 110 tracks, one
outcome a track:

| | Engine DJ | rekordbox Normal (101) | Seabass |
|---|---|---|---|
| Within 3 ms, steady | 10 | 10 | 33 |
| 3 to 10 ms, steady | 18 | 16 | 15 |
| Within 10 ms, drifts over 10 ms | 18 | 19 | 28 |
| On the beat, over 10 ms off | 33 | 30 | 4 |
| Half a beat out | 2 | 7 | 1 |
| Another tempo | 29 | 19 | 29 |
| Nine in ten of rekordbox's beats within 10 ms | 3 | 3 | 11 |
| Half of rekordbox's beats within 10 ms | 36 | 36 | 62 |

On the tracks at rekordbox's tempo and on its beat, Engine's grid is
4.3 ms late at the median and anywhere between 26 ms early and 39 ms
late (5th to 95th percentile); ours is 1.2 ms late, between 5 early and
7 late. On the tempo the table is not fair to Engine: it was run with a
tempo range of 78 to 155 (read from its settings afterwards), rekordbox
and Seabass with 70 to 180, and 18 of the 110 tracks have a rekordbox
tempo outside 78 to 155. Of Engine's 29 tracks at another tempo, 14 are
those, an octave apart: the range speaking. On equal terms, with Seabass
run with 78 to 155 as well and a tempo counted right when it is
rekordbox's brought into that range by octaves, Engine has the tempo
right on 97 of 110 and Seabass on 92; 10 tracks are wrong in both.
Seabass run with that range has Engine's tempo on 91 of 110. So on
the tempo of varied music Engine is ahead by five tracks, and where the
beats are set it is behind.

Against rekordbox's Normal grids (101 tracks), which are straight like
Engine's: within 10 ms and steady Engine 18, Seabass 22; another tempo
24 and 29; half a beat out 9 and 7. Seabass against Engine itself: the
same tempo on 79 of 110, within 10 ms and steady on 34.

Engine is a third opinion here and not a reference: what it shows is
that on this music two commercial analysers agree with each other no
better than ours agrees with either.

## A third opinion: Beat This!

The neural tracker of "What the literature says" was run over both
sets (a private environment beside the benchmark; nothing of it is in
the app). It places beats to 20 ms on another decoder's timeline, so it
says whose tempo and whose beat it has, never whose milliseconds: a
grid has its opinion when the tempo is the same and over six in ten of
its beats are within a fifth of a beat of the grid's.

How far its word goes, on the tuning half of the collection (751
tracks):

- Where Seabass and rekordbox agree (671 tracks), it is with both on
  662 and with neither on 9.
- Where Mixxx's method is half a beat from rekordbox (317 tracks), it
  is with rekordbox on 288, with Mixxx on 21, with neither on 8. So it
  does not lean away from rekordbox.
- Where Seabass is half a beat from rekordbox (33 tracks), it is with
  Seabass on 28, with rekordbox on 3, with neither on 2.
- Where Seabass has another tempo (7 tracks), it has rekordbox's tempo
  on 5 and ours on none.

Read together: on some 25 tracks of 751 rekordbox itself has the grid
between the beats, and those are most of what is left of "half a beat
out"; on the tempo rekordbox is right and we are not. On the varied
music (110 tracks, another tempo on 29) it has rekordbox's tempo and
beat on 14 of the 29, ours on 3, neither on 11.

None of this moves a number: rekordbox stays the reference in every
table. The tracks where the audio sides with our grid (the kick band
and this tracker for those half a beat out, the start of the hit for
those over 10 ms off: 46 of the 91 that differ on the tuning half) are
listed with pictures for Sebastian to hear and approve or not; the list
is with the private audio.

## More music, and what it showed

Two more sets came on 24 November, both analysed by rekordbox in its
Dynamic mode with the range 70 to 180, and split into halves like the
others; only the tuning halves have been run.

- **The second playlist**: 233 files of the 226 tracks asked for to fill
  what the first lacked (music that can be read at two tempos, drummers,
  tempo changes, threes, and what a DJ plays that the collection has
  little of); 126 scored tracks in the tuning half.
- **CD rips**: 731 files, up to three albums of each of 326 artists,
  picked by a rule and not by ear: rock, electronic, pop, hip-hop, jazz.
  368 in the tuning half.

With the analyser as it stood (the tempo range commit), the CD rips
were the worst it had met: another tempo than rekordbox's on 114 of
368, where the raw tracker of the Mixxx baseline has 60. Most of the
114 were at four thirds or two thirds of rekordbox's tempo, and every
one of those on a track whose tempo wanders by over a percent (5th to
95th percentile of the local period); not one on a track that holds.
The cause: a drummer who wanders smears the fold of the whole track at
every period alike, the candidates' folds come out nearly as flat as
each other, and the preference for 128 BPM decides alone.

Two changes followed.

- **The tempo's candidates are compared window by window**: every 8 s
  of the track folded at a period of its own within 5 percent of the
  candidate's, and the mean of those folds' peaks. One half of each
  window says where the peak is and the other how high it is there,
  because the highest point of a fold of few beats is partly chance,
  more so the fewer the beats, and a plain window peak had the slower
  candidates win (seven tracks of the CD rips went from right to half).
  Taking the wandering out of the track first, by the tempo curve, and
  then folding the whole of it was tried and did nothing.
- **A drummer is followed by a path, not by windows**: the beats as the
  path through the onset function that scores best with steps near the
  tempo curve's period (Ellis, "Beat Tracking by Dynamic Programming",
  2007), stretches with nothing to go by spaced evenly with as many
  beats as the curve says, each beat then set on its own onset as
  before. Only where the curve says the tempo wanders by over a percent.
  For every followed track it was better still on the CD rips (154
  within 10 ms and steady for 144) and worse on the collection: on a
  drum machine's track the path leaves the beat for whatever is
  loudest and sits between the beats for whole sections, and 15 of the
  72 grids Sebastian had approved (below) came out as another grid.
  Two other tests for "drummer or machine" (how well a straight grid
  fits the path's sure beats; how sure the window follower is of
  itself) told the two apart no better than the curve.

Tuning halves, before and after (tempo range commit; these two
changes):

| | Another tempo | Within 10 ms, steady |
|---|---|---|
| Collection, 751 | 7; 6 | 660; 659 |
| First variety list, 110 | 29; 26 | 48; 54 |
| Second playlist, 126 | 36; 31 | 58; 64 |
| CD rips, 368 | 114; 86 | 127; 144 |

On the CD rips half of rekordbox's beats are within 10 ms of ours on
52 percent of the tracks (45 before; the Mixxx raw tracker 60), nine in
ten on 9 percent (the raw tracker 2). The raw tracker still has the
tempo on more of them (301 of 369 for 278): on rock we are behind it
there and ahead of it in where the beats are set.

In the tests: a drummer who speeds up and slows down among notes off
his grid (the tempo by windows, and the path), and a drum machine with
loud stabs between the beats for sections (the windows, not the path).
Two rules have no test that turns red without them and rest on the
music alone: the two halves of a window checking each other, and the
15 ms over which the path takes its onsets.

## Sebastian's decisions on the tracks that differ

Every one of the 91 tracks of the collection's tuning half on which
the analyser's grid differed from rekordbox's (half a beat out, over
10 ms off, a drift, another tempo) was put before Sebastian as
pictures of the audio under both grids, with an answer to tick. The
answers are kept by row id with the private audio's notes.

| | Ours | Both fine | rekordbox | Neither | Cannot tell |
|---|---|---|---|---|---|
| Half a beat out, 33 | 28 | 1 | 3 | 0 | 1 |
| Over 10 ms off, 40 | 31 | 0 | 4 | 4 | 1 |
| Drifts, 11 | 9 | 0 | 1 | 1 | 0 |
| Another tempo, 7 | 3 | 0 | 3 | 1 | 0 |

With rekordbox or approved, the analyser of the tempo range commit:
732 of 751 (97.5 percent); with the two changes above 730, one
approved grid lost (a track at another tempo than rekordbox's whose
tempo the window comparison now reads as rekordbox does). An approval
counts for a run only while it has the grid he saw: the same kind of
difference, the median within 3 ms, a drift within 10 ms. rekordbox
stays the reference in every table; the approved tracks are counted in
a row of their own.

What the pictures taught. The first two pages drew envelopes smoothed
at 60 Hz with no delay, which draws a hit that starts at 0 with its
level peaking at 0 and its rise peaking 8 ms before: "the grid belongs
at the foot of the rise" was 10 to 15 ms early there, and his answers
on the tracks over 10 ms off stood at 14 to 11. Drawn again from the
audio with nothing smoothed (the size of the analytic signal of each
band, laid over itself at every beat, seven pixels a millisecond, where
a hit leaves the floor within a millisecond of its start) they are 31
to 4. A reading rule for a picture is to be checked by putting a known
signal through the picture's own processing first.

Two witnesses beside him on the tracks half a beat apart: Beat This!
(ours on 28 of 33, rekordbox's on 3) and Denon's players (below: ours
on 16 of the 17 they have analysed, rekordbox's on none).

### The CD rips, and two grids of the collection that came out anew

On 24 November he looked at the tracks of the CD rips' tuning half that
the analyser of 373e328e has at another tempo than rekordbox's Dynamic
grid (86) or half a beat out (11), with the track laid over itself at
each grid's beats and three strips of eight beats, and answered for 95.

| How our grid stands to rekordbox's | Ours is right | rekordbox is right | He could not tell |
|---|---|---|---|
| Half its tempo | 21 | 5 | 2 |
| Double its tempo | 2 | 4 | 0 |
| Two thirds | 5 | 4 | 0 |
| One and a half times | 4 | 2 | 2 |
| Three quarters | 1 | 0 | 0 |
| Four thirds | 4 | 1 | 0 |
| A few percent apart | 11 | 4 | 1 |
| No simple relation | 5 | 1 | 5 |
| The same tempo, half a beat out | 10 | 1 | 0 |
| All | 63 | 22 | 10 |

So on this music a grid that differs from rekordbox's is ours to keep
on two tracks in three, and rekordbox's Dynamic analysis is no truth
for rock: with rekordbox or approved, the analyser is right on 207 of
the 368 tracks (144 within 10 ms and steady, 63 approved), and
rekordbox itself, taken as right wherever he did not say otherwise, on
295. The tracks over 10 ms off (33) and those that drift (94) he has
not looked at.

Where one tempo is double the other he chose the slower one on 25 of
32 tracks, between 70 and 92 BPM, and the faster one, between 155 and
179, on 7: rekordbox counts this music twice as fast as he would mix
it more often than we count it too slow. Beat This is no judge here:
of the 38 tracks at another tempo on which it has rekordbox's tempo he
gave 20 to ours and 15 to rekordbox.

What is left for the analyser on this set: 21 tracks at a wrong tempo
(5 at half, 4 at double, 4 at two thirds, 2 at one and a half times,
1 at four thirds, 4 a few percent off, 1 other) and 1 half a beat out.

Of the collection, two tracks had come out of that analyser with a grid
he had not seen: one at double rekordbox's tempo, and one he had
approved at another tempo that is now at rekordbox's tempo and half a
beat out. He approved both: with rekordbox or approved the collection
stands at 732 of 751 again, 73 of them approved, and what is left is
the 19 tracks of before (rekordbox right on 11, neither on 6, undecided
on 2).

### The second playlist

The same evening, the same pictures, for all 36 tracks of the second
playlist's tuning half at another tempo than rekordbox's (31) or half
a beat out (5).

| How our grid stands to rekordbox's | Ours is right | rekordbox is right | He could not tell |
|---|---|---|---|
| Half its tempo | 14 | 4 | 0 |
| Double its tempo | 2 | 0 | 0 |
| Two thirds | 2 | 2 | 0 |
| One and a half times | 1 | 0 | 0 |
| Four thirds | 0 | 1 | 0 |
| A few percent apart | 0 | 0 | 1 |
| No simple relation | 1 | 2 | 1 |
| The same tempo, half a beat out | 2 | 2 | 1 |
| All | 22 | 11 | 3 |

With rekordbox or approved the analyser is right on 86 of the 126
tracks (64 within 10 ms and steady, 22 approved); rekordbox itself,
taken as right wherever he did not say otherwise, on 101. Where one
tempo is double the other he again chose the slower one, between 80
and 89 BPM, on 14 of 20 tracks. Left for the analyser: 9 tracks at a
wrong tempo (4 at half, 2 at two thirds, 1 at four thirds, 2 other)
and 2 half a beat out.

### The first variety list

And all 27 tracks of the first variety list at another tempo than
rekordbox's (26) or half a beat out (1): ours is right on 15, both
grids are fine on 3, rekordbox's is right on 5, undecided on 4. With
rekordbox or approved the analyser is right on 72 of the 110 tracks
(54 within 10 ms and steady, 18 approved); rekordbox itself on 91.
Left for the analyser: 5 tracks at a wrong tempo (2 at half, 1 at
double, 2 with no simple relation).

Over the three sets of varied music that makes 160 tracks looked at,
of which ours is right or as good on 103, rekordbox's on 38, and 17
undecided; two were not answered.

### Drum and bass, corrected

The tables above are as he ticked them. Asked afterwards about the
drum and bass tracks, which he had given to our 87 BPM, he said that
drum and bass belongs at the faster tempo. Six tracks change to
rekordbox's grid: four of the second playlist and two of the first
variety list (one of them undecided before). That leaves the second
playlist at ours 18, rekordbox 15, undecided 3 (with rekordbox or
approved 82 of 126, rekordbox itself 105), and the first variety list
at ours 14, both 3, rekordbox 7, undecided 3 (71 of 110, rekordbox
itself 93); over the three sets ours is right or as good on 98 and
rekordbox's on 44.

Which of two tempos an octave apart he wants is not in the accents:
on the 62 such tracks, how unlike the odd and the even beats of the
faster grid are, and whether the pattern repeats every two or every
four of them, in three bands, tell his choice apart no better than
"always the slower one". What he wants goes by the music's kind.

### Heard, not seen: the pictures misjudge tempo

At a tempo that is neither double nor half of rekordbox's, he had
given 15 tracks to ours on which Beat This has rekordbox's tempo: two
analysers of different make against ours. For those he got excerpts
to listen to, 15 s from the first part of the track and 15 s from the
last, once with a click on rekordbox's beats and once with a click on
ours (the clips are his music and stay on his machines). By ear he gave
13 of the 15 to rekordbox, found both fine on one and neither right on
one. So the pictures, the track laid over itself at each grid's beats,
are a good judge of where on a hit a grid sits and of which of two
beats half a beat apart has the kick, and a poor one of tempo: a grid
at a wrong tempo that sits on a hit every few beats still draws a
start at its line.

After that the CD rips stand at ours 50, both 1, rekordbox 33, neither
1, undecided 10 (with rekordbox or approved 195 of 368, rekordbox
itself 307) and the second playlist at ours 16, rekordbox 17,
undecided 3 (80 of 126, rekordbox itself 107). The other 27 tracks he
gave to ours or to both from the pictures at such a tempo are with
him to be heard; until then they count as he ticked them.

Judged by his decision where he made one (ours: the approved grid must
be kept; rekordbox: its grid within 10 ms and steady) and by rekordbox
elsewhere, the analyser of 373e328e is right on 1078 of the 1330
tuning-half tracks of the four sets that can be judged: 732 of 743 of
the collection, 71 of 107 of the first variety list, 80 of 123 of the
second playlist, 195 of 357 of the CD rips. That is the measure for
the next round. Two things were tried against it and not kept.
Following the tempo more readily (a straight grid has to explain 85 or
92 percent of the track and not 75) gains four CD tracks and loses 17
and 37 of the collection. And most of what is left is not tempo: on
139 tracks of the varied music the two grids have the same tempo and
beat and run apart over the track, and where our grid follows the
tempo there, both sit as tightly on the hits as each other on 61 of
85 tracks; where we kept one straight grid, rekordbox's is the tighter
one on 38 of 87 and ours on 7.

### The rest heard as well

He then heard the other 27. Of all 42 tracks at a tempo that is
neither double nor half of rekordbox's which he had given to ours or to
both from the pictures, the ear leaves 2 with ours, finds both fine on
9, gives 27 to rekordbox, neither on 2, and 2 he cannot tell. Where the
analyser has such a tempo and rekordbox another, rekordbox is right:
these are the analyser's errors, not rekordbox's. What stays with ours
at another tempo is nearly all double or half, where the question is
which tempo he wants.

| After both rounds | Ours | Both | rekordbox | Neither | Undecided | With rekordbox or approved | rekordbox itself |
|---|---|---|---|---|---|---|---|
| First variety list | 8 | 2 | 13 | 0 | 4 | 64 of 110 | 98 |
| Second playlist | 14 | 2 | 17 | 0 | 3 | 80 of 126 | 109 |
| CD rips | 34 | 7 | 41 | 2 | 11 | 185 of 368 | 321 |

By the score that judges a run by his decisions the analyser of
373e328e is right on 1061 of 1327 tuning-half tracks (collection 732
of 743, first variety list 64 of 106, second playlist 80 of 123, CD
rips 185 of 355), and the version before it on 1018: the tempo by
windows and the path are still a gain by the corrected decisions. The
figures of the section before this one were taken after the first 15.

### The tempo's choice, tried again: no other weighting does better

With the decisions as the truth for tempo (rekordbox's, or ours where
he gave the track to ours), the candidates the analyser weighs were
written out for every track of the four tuning halves and other rules
tried on them offline. On the 1139 tracks that could be matched the
true tempo is among the candidates on 1103, and the rule as committed
(the fold window by window, times a preference around 128 BPM an
octave wide) picks it on 1065. Nothing tried picks it more often:

- the preference counted for more or less (to the power 0 to 1.5),
  around 110, 120 or 140 BPM, 0.7 or 1.4 octaves wide: 806 to 1059;
- first the family by the fold alone (the first guess and its doubles,
  its thirds, its quarters), then the octave by the preference: 1020
  to 1064;
- the fold at each candidate's own half, third, double, threefold and
  fourfold period added to its score, or deciding the family: 922 to
  1066.

So the errors that are left at two thirds, three quarters, four thirds
and one and a half times the tempo are not a matter of weights: how
sharply the track folds at a period and at the levels around it does
not tell the beat from those relatives on these tracks. It needs
another kind of evidence. One is at hand: the raw tracker of the Mixxx
baseline. By the same truth our tempo is right on 1251 of 1327 tracks
and the raw tracker's on 1220, and they are wrong on different tracks:
of the 76 we have wrong it has 40 right, of the 107 it has wrong we
have 71 right. An arbiter between the two would have to be right on
two disagreements in three to gain anything; none is known yet.

### When to follow: the two grids compared, and what "drifting" is

Most of what the score still counts as wrong is not tempo: on 139
tracks of the varied music our grid has rekordbox's tempo and beat,
sits within 10 ms of it at the middle of the track, and the two run
apart along it; on 43 more it sits over 10 ms off.

Which grid sits on the hits there was measured (of a grid's beats with
a strong onset within 50 ms, the share whose onset lies within 12 ms of
that grid's usual place; on the tracks where the two grids agree it
comes out alike for both on 168 of 191). Where our grid follows the
tempo, the two are alike on 61 of 85 drifting tracks, ours the tighter
on 10 and rekordbox's on 14. Where we kept one straight grid,
rekordbox's is the tighter on 38 of 87 and ours on 7.

So a run that followed every track was made and rules that choose per
track between the committed grid and the followed one were tried on
the pair, by the score above:

| Rule | Right of 1327 | Collection, of 743 |
|---|---|---|
| Followed under three quarters explained (as it was) | 1061 | 732 |
| Every track followed | 937 | 616 |
| Followed wherever the followed grid is tighter by 0.12 | 1068 | 731 |
| The straight grid's tightness alone, under 0.80, to 85 percent explained | 1066 | 732 |
| To 85 percent explained, and the followed grid tighter by a tenth | 1068 | 732 |

The last is in the analyser since commit 0b7cd1b2. It is a small
change: seven tracks of the tuning halves, and on the held-back halves
(totals only) the count within 10 ms stays at 826, one CD track more
and one track of the collection fewer, while three in four of
rekordbox's beats are within 10 ms of ours on five tracks more. One
CD track whose grid he had approved by ear comes out followed now and
is his to hear again.

What the drifting tracks are came out of preparing excerpts for him to
hear: in the 15 seconds where the two grids are furthest apart they
are not a few tens of milliseconds apart but, on 16 of a sample of 20
tracks, 100 to 330 ms: for a stretch one of the two grids is on the
off-beat or between the beats, and comes back. Both sit on hits there,
which is why the tightness is alike. Whose grid slips is for his ear
(the sample is with him); if it is ours, a followed grid wants a check
of each stretch against the stretches around it.

### Whose grid leaves the beat, and three things that did not mend it

Beat This was asked as the third opinion on the 134 drifting tracks of
the three sets of varied music: in each 15 seconds where the two grids
are 60 ms or more apart on the middle beat, which grid has its beats
(over 70 percent of them within 40 ms of one of the grid's).

| On the drifting tracks | Our grid leaves the beat | rekordbox's does | Unclear | Never that far apart |
|---|---|---|---|---|
| Where our grid follows the tempo (86) | 48 | 7 | 23 | 8 |
| Where ours is one straight grid (48) | 9 | 16 | 14 | 9 |

So it is mostly our followed grid that leaves the beat for a stretch
and comes back, on about a fifth of a track's stretches, and where we
keep a straight grid it is more often rekordbox's Dynamic one that
does. It is not yet confirmed by his ear; a sample of 20 is with him.

What those stretches are, on the 86: 574 of them, four or five a
track, twelve beats long at the middle. In 278 our grid runs at a
tempo a few percent off rekordbox's there, which is a grid that has
let go and floats until it locks on again; in 255 it holds the tempo
with the phase a quarter, a third or half a beat off; in 41 it is at
four thirds or one and a half times the tempo for the stretch.

Three things were tried against it and none kept:

- putting sections back that lie between two steps of about half a
  beat, on the beat lists alone (130 sections on 58 tracks): 1066 for
  1068 by the decisions, and our grid still leaves the beat on 46;
- a stiffer path (its tightness 300 and 1000 for 100): 1067 and 1061,
  our grid leaving the beat on 43 and 45;
- more of the path's beats counted as unsure and spaced evenly (under
  half of what is strong, not a quarter): 1064, and 49.

The slips are not clean steps and not a matter of the path's
constants. What is left to try is in the following itself: a stretch
where the grid has little to go by keeps the tempo and the phase of
what is around it, and takes a new phase only from a stretch that is
sure of it.

### The import uses the analysis: phases 4, 5 and 6 begun

The track import (branch `track-import`, built on this one) analyses
every file it puts on a stick. So that it gets each improvement by
rebasing, the analysis has one home, here, and one call:
`domain::analyseAudio()` takes a track's samples and the tempo range
and gives the grid, its tempo as one number, the key and the waveform.

- **Phase 4, a first version.** `domain/waveform_from_audio` (now the
  library's `waveform.hpp`, see "The analysis as a library of its
  own"): three
  bands split at 150 Hz and 2 kHz, 150 entries a second, written for the
  import and moved here. The bytes of rekordbox's PWV7 and PWV6 are low,
  mid, high in that order; the public description of the format has
  mid, high, low. Measured on 8 tracks: byte 0 follows the level below
  150 Hz (0.87), byte 1 the middle (0.86), byte 2 above 2 kHz (0.88).
- **Phase 6, a first version.** `infrastructure/rekordbox/anlz_builder`:
  complete .DAT, .EXT and .2EX files from nothing, a tempo for every
  beat. The colours of PWV4 and PWV5 and the values of PWVC are guesses.
- **Phase 5, a first version.** `domain/key_from_audio` (now the
  library's `key.hpp`): the intake's
  prototype in C++, its learned weights as they are (chroma from the
  peaks of the spectrum in thirds of a semitone over five octaves, one
  weight an octave and note for major and for minor, fitted to
  rekordbox's keys on the tuning halves). The port gives the
  prototype's key on 2,550 of 2,551 tracks. Held-back halves, the same
  key as rekordbox: 848 of 1,245 (68.1 percent), where Mixxx's detector
  has 53.6 percent; by set 68.6 (collection), 67.2, 67.9 and 67.4. Of
  what it misses on the collection's held-back half, 52 are a fifth
  away, 57 the parallel key, 7 the relative, 110 something else.
- **The tempo range** is a setting (`music/analysisTempoRange`, four
  ranges, 70 to 180 when nothing is set), in Preferences under
  Experimental.

**Nothing is lost on the way to the stick.** The 751 tracks of the
collection's tuning half were imported into a folder standing in for a
stick with the import's command line tool: 703 imported (48 are the
same file as another), all with a grid, 28 of them followed, in 19
minutes. Read back, the rekordbox grid on the stick is a fresh
analysis's grid on every one of the 703: the same tempo, every beat
within 1 ms (the file holds whole milliseconds), the MP3s' place on
rekordbox's timeline included; and the Engine grid is the rekordbox
grid, every beat within 2 ms.

### When to follow, once more: nothing left in the rule

Every track run with its straight grid only (`st1`), so that a rule
which follows fewer tracks can be scored without the audio. By the
decisions: as committed 1068, never following 977 (collection 725 for
732), and the best rule found, not to follow where the straight grid
already sits tight on the hits, 1069 with the collection at 733 and one
approved grid lost. Not built.

Of the 7 collection tracks we follow and he gave to rekordbox, our own
straight grid is right on 2; the other 5 are wrong either way. What is
left on the collection are 11 single tracks, not a rule.

### rekordbox's straight grids to the microsecond: read 0.5 ms early, and put right

PQTZ holds a beat in whole milliseconds; the .EXT's PQT2 holds the rest,
in thousandths of one. For a grid of rekordbox's Normal analysis that
rest is real: with it the beats lie on one line to 0.05 ms (300 tracks;
with PQTZ alone to 0.53 ms), and it is 0.5 ms at the mean. For a grid of
its Dynamic analysis it is nothing: nearly every beat's rest is zero
(0.003 ms at the mean on the CD rips), those grids are on whole
milliseconds as rekordbox means them.

So the collection's reference was read half a millisecond early, the
three sets of varied music were read rightly. The benchmark now adds
PQT2 where a list names an analysis file. Against rekordbox's beats as
it means them, the committed analyser (run `h1`) had 547 of the 751
tuning tracks within 3 ms and not 591, its straight grids 1.9 ms early
at the middle and not 1.4.

Calibrated again by the rule as it was (the middle track about 1.5 ms
early, nine in ten early, no more late by over 3 ms than before): a
straight grid is given out 1.2 ms later than the place the onsets peak
at, where it was 0.7. A followed grid stays 3 ms earlier: that was
measured against Dynamic grids. Run `h3`:

| Collection, straight grids against rekordbox's real beats | Before | After |
|---|---|---|
| Tuning half, within 3 ms (of 751) | 547 | 590 |
| Tuning half, 3 to 10 ms | 112 | 69 |
| The middle track | 1.9 ms early | 1.4 ms early |
| Later than rekordbox, of those near it | 5 percent | 10 percent |
| Held-back half, within 3 ms (of 724) | not run | 566 |

By the decisions the collection stays at 732 of 743. The varied music
loses three tracks at the 10 ms line (1065 for 1068 over all four sets;
held back, within 3 ms: second playlist 31 for 31, CD rips 103 for
107): our straight grids there are measured against Dynamic grids,
which sit earlier than rekordbox's own straight ones, and are now half
a millisecond further from them.

Two things that follow and are not done here. The scoring by decisions
compared a half-beat grid's offset as a number, where minus and plus
half a beat are the same grid; it now compares around the beat. And the
app's own reader of rekordbox grids (`readTrackAnalysis`) still takes
PQTZ alone, so a grid copied from rekordbox to Engine arrives half a
millisecond early at the mean; changing it changes what the Beat Grids
page calls "already the same" for every grid copied so far, which is
for Sebastian to decide.

### A new playlist through the import, and: in doubt the double

A playlist of 280 MP3s of indie and electro, 18 hours, music the
analyser never saw, imported onto a stick by the test build and analysed
by rekordbox 7 in its Dynamic mode. What the import wrote is what the
analyser gives: the grid on the stick is a fresh analysis's on all 280,
every beat within 1 ms, and so is the key.

| The stick's grid against rekordbox's | Tracks | |
|---|---|---|
| Within 3 ms, start to end | 94 | 33.6 percent |
| 3 to 10 ms, start to end | 89 | 31.8 percent |
| The same beat, over 10 ms apart in places | 60 | 21.4 percent |
| The same beat, over 10 ms off throughout | 2 | |
| Half a beat out | 11 | 3.9 percent |
| Another tempo | 24 | 8.6 percent |

The key is rekordbox's on 189 (67.5 percent; 68.1 on the held-back
halves before). Our straight grids sit 3.2 ms after rekordbox's Dynamic
grid at the middle (later on 138 of 149), the followed ones 0.9 ms
before it.

**Heard (59 tracks, 2026-11-25).** Of the 24 at another tempo: rekordbox
21, ours 1, neither 2. Eighteen of the 24 are at half of rekordbox's
tempo (rekordbox 150 to 182 BPM, ours 75 to 91), and of those rekordbox
is right on 17. Half a beat out, 11: ours 6, both 1, rekordbox 2,
neither 1, not sure 1. Over 10 ms off, 2: rekordbox 1, neither 1. The
ten straight grids latest against rekordbox's (6 to 10 ms): both fine 6,
rekordbox 2, ours 1, not sure 1. Twelve of the sixty that run apart:
both fine 5, not sure 5, one each. So by ear we are wrong on 28 of the
280 and rekordbox on 11, and what runs apart is mostly two grids that
are both fine.

**The tempo range does part of it.** Analysed in 88 to 175, the playlist
has rekordbox's tempo on 268 tracks for 256: 14 of his 21 come right and
4 others are lost.

**A tempo or its double, counted.** On the tuning halves (the four sets
and this playlist's), where both lie in the range: where we take the
slow one it is the wrong one on 22 tracks of 68, where we take the fast
one on 3 of 247. The mistake has one side.

**What does not tell them apart.** What sits on the beats and between
them in three bands (a snare between the kicks, how alike consecutive
beats are): truly slow tracks alternate more from beat to beat (1.37 at
the middle for 1.09) and have less between the beats (0.68 for 0.87),
but fitted with one track left out at a time it moves as many tracks
wrongly as rightly (7 and 8), with the folds' ratio added too.

**In doubt the double** (his word, 2026-11-25: more beat lines help the
DJ). Where the double of the chosen tempo lies in the range and the
track folds there at least three quarters as sharply, it is the tempo.
Three quarters is where it stops paying: scored from a lab run that
printed every tempo tried, 30 tracks gained and 6 lost at 0.75, 42 and
15 at 0.65, 45 and 23 with no limit. Run `d2`, tempo right, with
rekordbox's tempo as the right one and his ear on this playlist:

| | Before | After | Gained | Lost |
|---|---|---|---|---|
| Tuning halves (1,108 tracks) | 1,032 | 1,052 | 26 | 6 |
| Held-back halves (1,057) | 1,000 | 1,010 | 14 | 4 |

On the playlist itself, all 280: another tempo 24 to 9, within 10 ms
start to end 183 to 195, wrong by ear 28 to 17.

**It goes against what he heard on 2026-11-24.** With those answers
counted the tuning halves stand at 1,052 for 1,056 (14 gained, 18 lost),
and the scoring by decisions at 1,045 of 1,327 for 1,065: on the CD rips
he gave 15 of the tracks now doubled to our slower tempo against
rekordbox's double (second playlist 4, first variety list 2, collection
1). He trusts the later round more and chose the double; the rule
follows that. If the earlier answers are to stand, the rule is too
strong and nothing found so far tells the two kinds of track apart.

Left on the playlist's tempo: four tracks still at half (their double
folds under three quarters as sharply), and five at four thirds, two
thirds or further off.

### What an analysis costs

Measured in an optimised build, one track at a time, a track of four
minutes at the middle: decoding 0.3 s, the grid 1.47 s, the key 0.68 s,
the waveform about 0.1 s. (The build the suite runs in is a Debug one
with this file alone optimised: its times for the grid are real, its
times for the key and for decoding three times too long.)

Over half of the grid's time was its band filters, each pass over the
whole track for one filter, and a tenth the smoothing of the folds,
which worked out where every step lies by two divisions. With every
sample through all the filters of a band in one pass each way, and the
smoothing without the divisions away from the ends, the grid takes
0.77 s, and on the 280 tracks of the indie playlist every beat is the
same as before.

What is left, largest first: the band filters still (0.35 s; the bands
could run beside each other, or the low bands at a lower rate, which
changes the numbers a little), the key's transforms (0.45 s; half of it
would go with a transform made for real input, with the same proviso),
reading the decoded audio, the grid's own transforms.

### The analysis as a library of its own

The code that listens to the music (grid, key, waveform, and the one
call that runs them) is in `lib/musicanalyze` now: a directory with its
own build file, C++20 and the standard library, nothing from the rest
of this tree. So that another program can use it, Mixxx say, by carrying
the directory; its README says how. Seabass reaches it as before:
`domain::analyseAudio()` and the `domain/` headers are still there and
are the library's types and calls under the names Seabass had for them
(a beat, `domain::Beat`, is the library's beat). `SeabassGridAnalyser`,
the analyser behind the port the benchmark runs every analyser through,
reads a source to its end and hands the samples over.

It was a move and nothing else: every track of the five sets analysed
before and after, 2,831 of them (one file of the CD rips cannot be
decoded), and every beat, every key and every other number the
benchmark writes is the same.

What came with it:

- **A version** (`version.hpp`, 0.1.0) for the interface.
- **A revision for each part** (grid, key, waveform, each at 1): a
  number that goes up with every change after which the same audio can
  give another result, and with no other. `Analysis::revisions` carries
  them with a result, for the import to keep beside what it writes, so
  that it is known later which code analysed a track and which tracks
  an improved analyser would do differently. (A grid a DJ has corrected
  by hand is not to be analysed again on those grounds: the import's
  ledger keeps a fingerprint of the grid it wrote for that.)
- **A check that a revision is not forgotten.** The library's own test
  (`musicanalyze_test`, which needs nothing but the library) generates
  audio, and `tests/results.txt` holds what each part makes of it under
  the revision it was written with: every beat of five grids, their
  tempo, two keys and their margins, the levels of two waveforms. A
  result that has changed under a revision that has not fails, and the
  file cannot be written again until the number is raised. Four builds
  (GCC with and without optimisation, Clang, Clang with fused
  multiply-add as on ARM) write the same digits, every beat on the same
  thousandth of a millisecond, so the room a number has is small: 0.005
  ms a beat. Checked red: straight grids given out 0.01 ms later with
  the revision left alone (fails, and the write is refused), the
  revision raised with the file left alone (fails), one beat of a
  followed grid moved by 0.05 ms (fails), the waveform's upper edge at
  2010 Hz for 2000 (fails). It is a reminder and not a proof: it sees a
  change on the audio the test makes, and music has more in it.

Not moved yet: the large tests of the analysers
(`tests/seabass_grid_analyser_test.cpp`, `key_from_audio_test.cpp`,
`waveform_from_audio_test.cpp`) stay in Seabass's suite, because they
use the generator of synthetic tracks in `tools/synth_track`; they run
against the library as they did against the code where it was.

Next in the library: audio in pieces. An analysis holds the whole track
(about 32 MB a minute of music at its peak, by the code and not yet
measured), which is what limits the import to mixes of an hour and a
half. Only the first stage of the grid reads the audio itself, and its
filters that run backwards through the track can run with a few seconds
of what comes next instead of all of it; the key and the waveform go
from front to back already.

## Denon again: the new music, and the players

**Engine DJ Desktop on the new sets** (tuning halves; again one
straight grid on every file; its range 78 to 155). Within 10 ms of
rekordbox's Dynamic grids and steady: second playlist 35 of 126 (ours
64), CD rips 67 of 368 (ours 144). The tempo on equal terms, all
brought to 78 to 155 by octaves: first variety list Engine 97 of 110,
ours 94; second playlist 115 of 126 and 114; CD rips 326 of 368 and
315 (282 before the two changes above). Engine is ahead on the tempo
by a few tracks everywhere, and behind in where the beats are set.

**The players on techno.** A backup of the stick WHALESHARK holds the
grids the players (Prime 4, Go+) made for 953 tracks, 373 of them in
the collection's tuning half; all straight. They are Denon's own
analysis and not rekordbox's taken over: against rekordbox they spread
by milliseconds.

| On those 373 tracks, against rekordbox | Denon's players | Seabass |
|---|---|---|
| Within 3 ms, steady | 213 | 301 |
| 3 to 10 ms, steady | 96 | 38 |
| Over 10 ms off | 34 | 11 |
| Half a beat out | 23 | 17 |
| Another tempo | 0 | 2 |
| Right by Sebastian's decision, else by rekordbox | 327 | 367 |

Denon's grid sits +0.06 ms from rekordbox's at the median, half of the
tracks between -1.9 and +1.8 ms. No track at another tempo: on techno
the players' tempo is not in question.

## The held-back halves, run once

The collection, the second playlist and the CD rips were each split in
two before any tuning; the analyser was tuned on one half only. On 24
November the other halves were run once, with the analyser of commit
373e328e. Only the totals below were read off these runs: no track of
a held-back half was opened, so they can judge a later version too.

| Set | Analyser | Half | Tracks | Within 3 ms, steady | Within 10 ms, steady | Half a beat out | Another tempo | 3 in 4 beats within 10 ms |
|---|---|---|---|---|---|---|---|---|
| Collection | Mixxx method | tuning | 751 | 163 | 361 | 317 | 18 | |
| Collection | Mixxx method | held back | 724 | 188 | 371 | 289 | 11 | |
| Collection | Seabass | tuning | 751 | 591 | 659 | 34 | 6 | |
| Collection | Seabass | held back | 724 | 566 | 627 | 36 | 6 | |
| Second playlist | Mixxx method | tuning | 126 | 17 | 38 | 6 | 30 | 19.8 % |
| Second playlist | Mixxx method | held back | 105 | 12 | 22 | 7 | 25 | 20.0 % |
| Second playlist | Mixxx raw tracker | tuning | 126 | 28 | 57 | 5 | 25 | 25.4 % |
| Second playlist | Mixxx raw tracker | held back | 105 | 18 | 43 | 6 | 21 | 22.9 % |
| Second playlist | Seabass | tuning | 126 | 43 | 64 | 5 | 31 | 33.3 % |
| Second playlist | Seabass | held back | 105 | 31 | 48 | 3 | 18 | 36.2 % |
| CD rips | Mixxx method | tuning | 368 | 27 | 55 | 20 | 80 | 13.6 % |
| CD rips | Mixxx method | held back | 362 | 30 | 67 | 12 | 68 | 13.5 % |
| CD rips | Mixxx raw tracker | tuning | 368 | 84 | 150 | 21 | 60 | 25.5 % |
| CD rips | Mixxx raw tracker | held back | 362 | 79 | 162 | 12 | 61 | 25.4 % |
| CD rips | Seabass | tuning | 368 | 111 | 144 | 11 | 86 | 32.9 % |
| CD rips | Seabass | held back | 362 | 107 | 151 | 9 | 79 | 31.8 % |

The tuning holds on music the analyser has not seen. On the collection
it is within 3 ms of rekordbox on 78.7 % of the tuning half and 78.2 %
of the held-back half, within 10 ms on 87.7 % and 86.6 %, at another
tempo on six tracks of each. On the CD rips the held-back half comes
out a little better than the tuning half (within 10 ms 41.7 % for
39.1 %, another tempo 21.8 % for 23.4 %), on the second playlist a
little worse within 10 ms (45.7 % for 50.8 %) and better on tempo
(17.1 % for 24.6 %); the baseline's rows move the same way there, so
that is the music of the half and not the tuning.

What the held-back halves confirm as well: on the CD rips the raw
tracker of the Mixxx baseline still has rekordbox's tempo on more
tracks than we have (another tempo on 61 for our 79) and is within
10 ms on more (162 for 151), while beat by beat ours is ahead (three
in four beats on 31.8 % of the tracks for 25.4 %).

## Phase 3, in order

1. Done: who is right on the tracks half a beat apart (above). The
   worklist of every track that differs, by kind and by row id, is with
   the private audio.
2. The analyser: the tempo over the whole track to a few thousandths of
   a BPM, the phase from the low band with both half-beat phases
   compared, the beat refined on the transient to the sample, a
   confidence so that an unsure track is flagged. Tuned on the tuning
   half of the collection, judged on the held-back half and on the
   generated tracks.
3. A reference for the music the collection does not have. rekordbox
   is none there; the generated tracks are exact but are not that
   music. Until there is one, the variety list scores the tempo and the
   side of the beat on the tracks where rekordbox agrees with itself.
4. The same tables after every change. A change that moves no number
   is not kept.
