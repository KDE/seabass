<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# musicanalyze

Finds the beat grid, the tempo, the musical key and a three-band waveform
of a track from its decoded audio. Written for Seabass and kept apart
from it on purpose: C++20, the standard library, and no dependency on the
rest of this repository, so another project can carry the directory as it
is.

## Where it stands

Early, and still changing. It was built for DJ music and measured mostly
on electronic dance music, against the grids and keys rekordbox gives for
the same files; `docs/track-analysis-plan.md` in the Seabass repository
says how it was measured, what it gets right and what it gets wrong.
Known limits:

- It does not say which beat starts the bar (`beatInBar` is 0).
- Whether a track is at a tempo or at its double is a choice more than a
  fact; a tempo range steers it, and in doubt the double is taken.
- The key is a first version.
- It wants the whole track in memory at once. Taking the audio in pieces
  is planned; the waveform does already (`WaveformMeter`).

## Use

```cpp
#include <musicanalyze/analysis.hpp>

// Mono samples, full scale 1.0, and their rate.
const musicanalyze::Analysis found = musicanalyze::analyse(samples, 44100);

for (const musicanalyze::Beat &beat : found.grid.beats) {
    // beat.timeMs, from the first sample
}
// found.bpm, found.grid.followsTempo, found.grid.error
// found.key.key (with musicanalyze::keyName()), found.key.margin
// found.waveform.entries: 150 a second, low / mid / high / overall
// found.revisions: which code made each part
```

Each part can be called on its own: `gridFromSamples()` (grid.hpp),
`keyFromSamples()` (key.hpp), `waveformFromSamples()` (waveform.hpp). The
headers say what each listens to and how it decides. Decoding is the
caller's: the library takes samples, not files.

## Building it into a project

Any of:

- `add_subdirectory(musicanalyze)` and link `musicanalyze::musicanalyze`.
- Compile the five files in `src/` with `include/` on the include path.
  There is no generated header and no configure step.
- Build and install it on its own (`cmake -S . -B build && cmake --build
  build && cmake --install build`), then `find_package(musicanalyze)`.

It is a static library for now. It compiles as strict C++20 whatever the
project around it uses.

## Versions and revisions

Two things are numbered, in `include/musicanalyze/version.hpp`:

- The library's **version** (0.1.0) is about its interface. Before 1.0 a
  new minor version may change it.
- A **revision** each for the grid, the key and the waveform is about
  results: it goes up with every change after which the same audio can
  give another result, and with no other. Store it beside a result and
  you know later which tracks an improved analyser would do differently,
  and which code a wrong grid came from.

## Tests

`musicanalyze_test` needs nothing but the library: it generates its own
audio, checks the interface, and compares the results with
`tests/results.txt`, which was written under the revisions it names. A
result that has changed under a revision that has not is a failure, so a
revision is hard to forget. After raising one, write the file again with
`musicanalyze_test --write tests/results.txt`. The file was written on
Linux on x86-64; each number has a little room for another compiler's
arithmetic, and if the test fails on your platform with the library
unchanged, that is worth a report.

The larger tests (drummers who wander, tempo ranges, tunings, sample
rates) are still in Seabass's own suite, `tests/seabass_grid_analyser_test.cpp`,
`tests/key_from_audio_test.cpp` and `tests/waveform_from_audio_test.cpp`,
because they use Seabass's generator of synthetic tracks.

## Licence

GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL, as Seabass.
Every file names its licence in its first lines; the texts are in
`LICENSES/` at the top of the Seabass repository and belong with a copy
of this directory.
