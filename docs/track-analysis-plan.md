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
