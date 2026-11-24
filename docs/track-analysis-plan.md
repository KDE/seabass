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
`domain/seabass_grid_analyser.hpp`; `--tempo-range LOW-HIGH` in the
benchmark), as rekordbox does with its "BPM range". A tempo found
outside it is given out as its double or half inside it. The default is
rekordbox's default, 70 to 180, with which every reference grid here was
made. The setting has no place in the app yet: the analyser is not
wired into it. When it is, the ranges to offer are rekordbox's: 45-90,
58-115, 68-135, 70-180, 78-155, 88-175, 98-195.

- A range narrower than an octave is widened at the top to one
  (rekordbox's own are a hair under: 88 to 175), and both ends have half
  a percent of room, so that a track at 70.00 BPM is at 70 in a range
  from 70.
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
