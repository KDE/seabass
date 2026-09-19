<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# The Music preferences, and what they reach

Three settings under *Preferences -> Music* decide what Seabass treats as
the same recording, and what it treats as a cue. They are the first
preferences in this app that reach past the GUI into the layers that read
and write libraries, so this is a note on how they get there and what
they change.

## The settings

| Setting | Default | Range |
|---|---|---|
| Same duration within *N* sec is considered an exact match | 2 s | 0 to 30 |
| Same duration within *N* sec will compare audio | 10 s | 0 to 120, never below the exact window |
| Ignore cues at 0:00 | on | |

The defaults are exactly what the code used as hardcoded constants before
any of this was a setting, so an existing library's next scan finds what
its last one found.

## How they reach the Qt-free layers

`domain::MatchingPolicy`, a process-wide value set by a composition root
and read everywhere. The GUI's `AppSettingsController` pushes all three
into it on construction and on every change; `seabass-cli` never sets it
and runs on the defaults.

This is the same shape as `paths::setLocalRootOverride()` and exists for
the same reason: a dozen call sites across four layers read these
numbers, several inside loops that have no business carrying a settings
object down to them, and threading three doubles through every signature
buys no behaviour a global does not.

Every accessor is a lock-free atomic, because scans run on QtConcurrent
threads while the Preferences page lives on the GUI thread. A change made
*during* a scan is picked up by whatever part of that scan has not run
yet. That is harmless (the answer is recomputed next scan either way) but
it is the reason not to treat a scan's numbers as one consistent
snapshot.

## What the exact-match window changes

Everything that compares two lengths to decide "same recording":

- `domain/track_matching.cpp` -- matching a track in one catalog to a
  track in another, which is what Sync Cue Points is built on
- `domain/duplicate_cleanup.cpp` -- whether a group's copies agree on
  length, which feeds `DuplicateCleanupPlan::differs`
- `domain/duplicate_cue_consolidation.cpp` -- `DuplicateTrackFinder`,
  which both Clean Up Duplicates and Match Duplicate Cues group with
- `infrastructure/local/local_cue_store.cpp` -- finding the stored row a
  stick track's cues belong to
- `infrastructure/local/metadata_store.cpp` -- the same, for the
  metadata backup

One number, one meaning, with **one documented exception**: the two local
stores use `MatchingPolicy::backupIdentitySeconds()`, which is
`min(setting, 2 s)`. They are capped because the two directions are not
symmetrical there. `LocalCueStore::upsert()` takes the first stored row
whose title+artist key matches inside the window, then DELETEs that row's
cues and writes the incoming ones; widen the window and a 3:00 radio edit
filed under the same artist and title as a 3:25 extended mix starts
matching it, so backing up one silently destroys the other's backed-up
cues, with nothing shown and nothing to undo. Narrowing costs at worst a
second stored row, which loses nothing, so a user who *tightens* the
setting is obeyed everywhere.

Everything else follows the setting, because every one of those paths
puts a plan in front of the user before anything is written.

Both directions are consequential, which is why the range stops at 30 s
and why nothing is ever written without asking. Wider finds more copies,
including edits that are not copies. Narrower misses real duplicates.

## What the compare-audio window changes

Only `DuplicateTrackFinder`, and only for a pair whose stored lengths are
further apart than the exact window and no further apart than this one.
For such a pair, both files are decoded, the leading and trailing silence
is measured, and the length of the music between them is compared with
the exact-match window.

The case it exists for: two copies of one recording routinely differ by
seconds of *silence*. An encoder pads, a rip keeps the run-out, a
re-export trims the intro. Take the silence off and the music is the same
length on both. A difference that survives the trim is a different edit,
and is not grouped.

What it is not: a fingerprint. It measures silence, so it cannot tell two
different recordings of equal length apart. That is why it is only ever
consulted about a pair that already agrees on artist and title, never as
a matcher in its own right.

### Cost, and the cache

Decoding is orders of magnitude more expensive than comparing two
numbers, so it runs only for pairs genuinely in doubt. What it finds is
written to `<stick>/Seabass/caches/silence.jsonl`, keyed on a path
relative to the stick root and guarded by the file's size and mtime, so:

- a second scan of the same stick decodes nothing
- a stick carried to another computer keeps its answers
- a re-ripped or re-tagged file is re-measured rather than answered from
  a figure belonging to its previous contents

`local::CachedAudioContentProbe` is a decorator over the real probe
rather than a container callers consult themselves, so no call site can
forget to use it.

### When it cannot run

`infrastructure::audio::makeAudioContentProbe()` returns null when the
wider window is not wider than the exact one (which is how "off" is
expressed) or when the build has no decoder and the stick has no cache.
"Has no decoder" is a run-time question, not an `#ifdef`: QtMultimedia can
be linked in while the FFmpeg plugin never loads, and
`QtMultimediaSilenceProbe::decodingAvailable()` is what Preferences and the
scan both ask.
`DuplicateTrackFinder` takes a null probe and compares stored lengths
alone, exactly as it did before any of this existed.

A build with no decoder says so on the Preferences page and on the Clean
Up page, rather than offering a number that does nothing.

### Two invariants worth not breaking

**A probe may only ever add.** The clustering runs in two passes: stored
lengths first, then the audio attaches only tracks the first pass left on
their own. A single greedy pass that consulted the probe inline could make
a pair the exact window had already found *disappear* when the setting was
switched on (A=300 s, B=306 s, C=308 s at 2 s/10 s: `{B, C}` becomes
`{A, B}`). `tests/matching_policy_test.cpp` cases 13 and 14 hold this, the
second as a property over 400 generated libraries.

**A refusal is not an answer.** Every "no answer" is no opinion, never a
match, so a cancelled scan, a missing decoder and a corrupt file all
degrade to the grouping stored lengths alone would give, never to a wrong
one. That is also what lets `CancellableAudioContentProbe` implement Cancel
by simply refusing to answer.

## What "Ignore cues at 0:00" changes

`domain::isJunkCue`, which is the single definition shared by the finder,
the remover, the metadata backup, the restore and the XML export. With it
off, a cue inside the first second is an ordinary cue everywhere: Library
Health stops offering it for cleanup, it is kept in backups, and it
counts when two catalogs are compared.

One exception, deliberate: a cue at a *negative* position stays junk
either way. That is a format's "no cue set" sentinel read back as a
position, with nowhere in the track to point, so "keep my cues at 0:00"
must not be read as "hand me a cue I cannot navigate to".

## Manual merge is always available

None of these numbers is the only route. Two tracks can be merged by hand
from Browse Library (the *Merge* button on a track, then *Merge with
another track*), which takes no notice of lengths at all. Both duplicate
pages say so in their own help, because a matcher that is tuned wrong in
either direction needs a way out that is not a setting.
