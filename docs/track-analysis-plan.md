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
first beat to last, of the truth and of the found grid.

| preset | analyser | median | scatter | 1 ms | 3 ms | 8 ms | 20 ms | tempo | shape | true BPM | found BPM |
|---|---|---|---|---|---|---|---|---|---|---|---|
| house-124 | qm-raw | -3.7 | 3.7 | 0.11 | 0.31 | 0.68 | 0.86 | same | moving | 124.000 | 124.483 |
| house-124 | mixxx-constant | -3.6 | 0.0 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 124.000 | 124.000 |
| house-124 | mixxx-constant-unrounded | -3.6 | 1.0 | 0.00 | 0.35 | 1.00 | 1.00 | same | constant | 124.000 | 124.004 |
| trance-137.98 | qm-raw | +0.1 | 210.4 | 0.06 | 0.16 | 0.35 | 0.43 | same | moving | 137.980 | 138.330 |
| trance-137.98 | mixxx-constant | +210.4 | 3.5 | 0.00 | 0.00 | 0.00 | 0.00 | same | constant | 137.980 | 138.000 |
| trance-137.98 | mixxx-constant-unrounded | +216.1 | 0.6 | 0.00 | 0.00 | 0.00 | 0.00 | same | constant | 137.980 | 137.977 |
| drift-122 | qm-raw | -4.3 | 3.8 | 0.11 | 0.31 | 0.65 | 0.79 | same | moving | 122.047 | 122.006 |
| drift-122 | mixxx-constant | -71.4 | 66.3 | 0.04 | 0.11 | 0.20 | 0.30 | same | constant | 122.047 | 121.667 |
| drift-122 | mixxx-constant-unrounded | -24.6 | 113.3 | 0.01 | 0.12 | 0.19 | 0.25 | same | constant | 122.047 | 121.561 |
| ramp-100-125 | qm-raw | -4.2 | 3.9 | 0.09 | 0.25 | 0.66 | 0.85 | same | moving | 115.216 | 115.887 |
| ramp-100-125 | mixxx-constant | -7.3 | 41.7 | 0.01 | 0.02 | 0.40 | 0.42 | other | constant | 115.216 | 125.000 |
| ramp-100-125 | mixxx-constant-unrounded | -6.9 | 40.4 | 0.01 | 0.01 | 0.40 | 0.42 | other | constant | 115.216 | 124.994 |
| step-120-126 | qm-raw | -3.3 | 4.0 | 0.14 | 0.38 | 0.64 | 0.86 | same | moving | 124.218 | 124.708 |
| step-120-126 | mixxx-constant | -6.2 | 71.4 | 0.00 | 0.00 | 0.32 | 0.35 | other | constant | 124.218 | 120.000 |
| step-120-126 | mixxx-constant-unrounded | -6.1 | 72.8 | 0.00 | 0.00 | 0.32 | 0.34 | other | constant | 124.218 | 119.996 |
| alternating-127.5-128 | qm-raw | -2.3 | 227.6 | 0.03 | 0.12 | 0.31 | 0.43 | same | moving | 127.715 | 128.048 |
| alternating-127.5-128 | mixxx-constant | -200.8 | 19.2 | 0.00 | 0.00 | 0.00 | 0.00 | same | constant | 127.715 | 127.667 |
| alternating-127.5-128 | mixxx-constant-unrounded | -206.4 | 17.2 | 0.00 | 0.00 | 0.00 | 0.00 | same | constant | 127.715 | 127.684 |
| live-drummer-96 | qm-raw | -1.8 | 4.4 | 0.12 | 0.39 | 0.74 | 0.99 | same | moving | 96.151 | 96.173 |
| live-drummer-96 | mixxx-constant | +1.1 | 101.7 | 0.03 | 0.09 | 0.23 | 0.32 | same | constant | 96.151 | 95.333 |
| live-drummer-96 | mixxx-constant-unrounded | +0.2 | 99.7 | 0.03 | 0.12 | 0.23 | 0.32 | same | constant | 96.151 | 95.316 |
| breakbeat-swing-132 | qm-raw | -1.2 | 3.8 | 0.13 | 0.38 | 0.75 | 0.81 | same | moving | 132.000 | 131.705 |
| breakbeat-swing-132 | mixxx-constant | -2.3 | 0.0 | 0.00 | 1.00 | 1.00 | 1.00 | same | constant | 132.000 | 132.000 |
| breakbeat-swing-132 | mixxx-constant-unrounded | -2.2 | 0.3 | 0.00 | 1.00 | 1.00 | 1.00 | same | constant | 132.000 | 131.999 |
| long-breakdown-126 | qm-raw | -3.4 | 6.2 | 0.08 | 0.21 | 0.48 | 0.60 | same | moving | 126.000 | 126.315 |
| long-breakdown-126 | mixxx-constant | -3.9 | 0.0 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 126.000 | 126.000 |
| long-breakdown-126 | mixxx-constant-unrounded | -3.9 | 0.1 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 126.000 | 126.000 |
| drumless-intro-128 | qm-raw | -3.7 | 8.6 | 0.07 | 0.17 | 0.39 | 0.53 | same | moving | 128.000 | 125.986 |
| drumless-intro-128 | mixxx-constant | -5.3 | 0.0 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 128.000 | 128.000 |
| drumless-intro-128 | mixxx-constant-unrounded | -2.4 | 3.5 | 0.15 | 0.43 | 0.90 | 1.00 | same | constant | 128.000 | 128.020 |
| house-124-48k | qm-raw | -3.2 | 3.5 | 0.14 | 0.33 | 0.72 | 0.85 | same | moving | 124.000 | 124.479 |
| house-124-48k | mixxx-constant | -3.3 | 0.0 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 124.000 | 124.000 |
| house-124-48k | mixxx-constant-unrounded | -3.3 | 0.1 | 0.00 | 0.00 | 1.00 | 1.00 | same | constant | 124.000 | 124.000 |
| halftime-174 | qm-raw | +1.3 | 172.4 | 0.07 | 0.20 | 0.43 | 0.50 | half | moving | 174.000 | 87.018 |
| halftime-174 | mixxx-constant | +341.7 | 0.0 | 0.00 | 0.00 | 0.50 | 0.50 | half | constant | 174.000 | 87.000 |
| halftime-174 | mixxx-constant-unrounded | +0.1 | 172.4 | 0.10 | 0.25 | 0.50 | 0.50 | half | constant | 174.000 | 87.011 |

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
  answer for a fractional track.

Key (`qm-key` against the preset's stated key): right on the two major
presets ramp-100-125 (D) and drumless-intro-128 (G); every minor preset
comes back as its relative major (A minor as C, C minor as E flat,
G minor as B flat, F minor as A flat, D minor as F), and E major as its
relative C sharp minor.

### Real music

Not run yet. `analysis_benchmark audio <stick> --analyser all --half tuning --jobs 3`
on a stick that holds a rekordbox export with its audio gives the tables
for this section; the held-back half stays unseen until an analyser is
tuned.
