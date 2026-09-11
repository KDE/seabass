# Write-path performance

How to find out where a Seabass save actually spends its time, what the
answer was the first time we asked, and the rules of thumb that came out of
it. Optimizing write paths against slow removable media is recurring work in
this project, so this file is the place to record each round of it rather
than re-deriving the same numbers from scratch.

The short version: **on a USB stick, per-item overhead beats bandwidth, and
CPU work you repeat per item beats both.** Measure before designing. The
first time we did this the obvious culprit (fsync latency) turned out to be
the smaller half, and the fix that mattered had nothing to do with I/O.

## The tools

Two standalone benchmarks, both Qt-free and both safe to point at a real
stick:

- `tools/stick_write_bench.cpp` — splits a save into per-item overhead
  (read-only) and the write strategy itself, comparing per-file flush against
  batched alternatives.
- `tools/onelibrary_tx_bench.cpp` — the cost of one OneLibrary cue write,
  in the shape the code uses today versus one held connection.
- `tools/engine_write_bench.cpp` — the same for Engine, timed on the stick
  and on a ramdisk so the split between device and CPU is visible, and
  against the proposed snapshot-and-update shape.

**Neither writes to the real library.** Everything goes into a scratch folder
under the stick root (`.seabass-writebench`, `.seabass-writebench-tx`) which
they create and delete; `PIONEER`, `Engine Library` and `.seabass-backups`
are only ever read. The OneLibrary tool works on a copy of `exportLibrary.db`
placed in that scratch folder, so it is the same stick and the same
filesystem but never the real file. Keep any new benchmark to that rule.

They are not CMake targets, for the same reason `tools/onelibrary_audit.cpp`
is not: they are occasional instruments, not part of the build. Compile them
against the already-built static library:

```
cmake --build build                                  # so libseabass_core.a exists
g++ -std=c++23 -O2 -I src -I third_party/kaitai_struct_cpp_stl_runtime \
    tools/stick_write_bench.cpp -o build/stick_write_bench \
    build/libseabass_core.a build/librekordbox_format.a \
    build/libkaitai_cpp_stl_runtime.a -lz -ldl -lpthread

build/stick_write_bench /media/you/STICK --device /dev/sdX1 10 50 100 200
build/onelibrary_tx_bench /media/you/STICK 50

# engine_write_bench also needs libdjinterop and its generated config header
g++ -std=c++23 -O2 -I src -I third_party/libdjinterop/include \
    -I build/third_party/libdjinterop/include \
    tools/engine_write_bench.cpp -o build/engine_write_bench \
    build/libseabass_core.a build/third_party/libdjinterop/libdjinterop.a \
    build/librekordbox_format.a build/libkaitai_cpp_stl_runtime.a \
    -lz -ldl -lpthread -lsqlite3
build/engine_write_bench /media/you/STICK 50
```

### Still not measured

- A whole Sync save end to end. The per-item costs above are components; no
  run has yet timed the real save loop over a few thousand staged plans.
  The harness for it would stage N changes against a *copy* of a library on
  the stick and run them through `runSaveLoop`, which is the only way to
  catch costs that live between the items rather than in them.
- The rekordbox ANLZ write in the Sync shape specifically, which unlike the
  stray-cue path has no OneLibrary mirror write beside it.
- Clean Up and Duplicates, whose per-item work is row removals and cue
  merges rather than a cue write, so the numbers here do not carry directly.

## Method, and the trap in it

A USB stick's write latency is not a constant. Its controller does wear
levelling and garbage collection on its own schedule, so a naive benchmark
measures the schedule, not the code.

The first run of `stick_write_bench` did exactly that: it ran the three
strategies in a fixed order and reported that the first one was **thirty-two
times slower per item** than the same strategy measured later in the same
run. The strategy that happens to go first absorbs the warm-up, and whatever
goes last can be charged for work the earlier ones queued in the controller.

So any benchmark against removable media in this project must:

1. **Do a discarded warm-up** before the first timed block.
2. **Rotate the order** of the things being compared across repetitions.
3. **Repeat at least three times and report the median**, and print the
   individual runs so the spread is visible rather than hidden.
4. **Sync the filesystem between blocks**, outside the timer, so one block's
   dirty pages are not charged to the next.

5. **Unmount and remount the stick before every timed run, then let it
   settle** (`--device /dev/sdX1`). This is the one that actually works.
   Each run then starts with a cold page cache and a freshly mounted
   filesystem, and the stick's controller does its background work during
   the idle gap instead of in the middle of a measurement.

With the first four only, expect run-to-run spread of two to seven times on
the write half. With the remount added it collapses to within a second:
three runs of one strategy at 200 cues came out 53.41 / 54.87 / 54.40. That
is the difference between a benchmark that can rank two strategies and one
that cannot. Report the spread either way; if two strategies sit inside it,
the benchmark has not distinguished them.

The remount also shifts every number upward, because a cold mount re-reads
filesystem metadata. That is not a distortion to correct for: a real save
interleaves reads and writes across a 300 MB library, so it runs under
memory pressure much closer to the cold case. The measured components add up
to 105 s of the observed 155 s under the cold condition, against 65 s warm.

Reads are easier: `posix_fadvise(POSIX_FADV_DONTNEED)` drops the page cache
for a file on Linux, which is what `stick_performance_probe.cpp` already uses.
There is no equivalent on Windows or macOS.

## Baseline: 2026-09-07, RV2

29 GB exFAT stick on a USB 3 link, 1161 rekordbox tracks, 1469 Engine tracks,
OneLibrary mirror present. Cue files: 1992 `.EXT` totalling 303 MB, mean
156 KB, plus 1992 `.DAT` totalling 16 MB that the cue path never touches.

The case under test: removing stray cues (0:00 memory cues), which are almost
always one per track, so N cues means N distinct analysis files. A live run
of 201 of them took **155 s**.

### Where the time goes, per cue

Rekordbox track, with Engine and OneLibrary also on the stick:

| Work done per cue | Now | If fixed | Why it repeats |
|---|---:|---:|---|
| OneLibrary cue write (two encrypted opens + one transaction) | 235.3 ms | 4.7 ms | A fresh writer per cue; each open re-derives the SQLCipher key from a passphrase |
| Analysis-path lookups (two full `export.pdb` parses) | 15.0 ms | 0.1 ms | Controller resolves the path, then the writer resolves it again |
| Staleness checksums (three whole-file CRC32) | 0.9 ms | 0.0 ms | Constructor, pre-write guard, post-write baseline |
| Cue file backup and rewrite (two durable writes) | 272.0 ms | 272.0 ms | Real work |
| **Measured total** | **523.2 ms** | **276.8 ms** | |

Reference points from the same run: reading all 31.3 MB of the touched cue
files takes 0.66 s cold; building the whole 1161-track id-to-path index in
one pass takes 0.02 s, against 8.4 ms for a single lookup; one SQLCipher open
with `PRAGMA key` costs 118.3 ms, and 0.6 ms once the connection is held.

Those components account for 105 s of the observed 155 s. The rest is
unmeasured work of the same shape (the Engine database reopened per cue, the
stick log opened and closed twice per cue, the backup store rewalking its own
growing directory per file, and a real delete-plus-inserts rather than the
benchmark's single no-op update). Record the gap rather than papering over it.

### The write strategies

Medians of three runs with rotated ordering, writes only, in seconds:

| Strategy | 10 | 50 | 100 | 200 | Syncs at 200 |
|---|---:|---:|---:|---:|---:|
| Per-file flush (today) | 15.37 | 37.25 | 45.59 | 54.40 | 800 |
| Relaxed, one sync at the end | 13.71 | 36.47 | 45.38 | 49.66 | 1 |
| Strict batch via tmpfs mirror | 14.25 | 35.27 | 45.64 | 50.38 | 3 |

**The three strategies are the same speed.** At 100 cues they are within
0.6% of each other; at 200, the widest gap, batching saves 8.7%; at 10 and
50 the ordering between them flips. An earlier warm-cache run appeared to
show batching winning by a factor of two -- that was the stick's mood, not
the code, and it is exactly what the remount exists to prevent. For the
record, that warm run was 1.08 / 3.32 / 6.94 / 14.20 for per-file flush,
1.09 / 2.34 / 4.46 / 6.34 relaxed, 0.55 / 4.89 / 6.43 / 12.16 batched.

Individual runs, to show the spread — this is why the medians above should
not be read to two decimal places:

| Strategy | 10 | 50 | 100 | 200 |
|---|---|---|---|---|
| Per-file flush | 12.81 / 15.52 / 15.37 | 40.25 / 36.85 / 37.25 | 46.19 / 45.50 / 45.59 | 53.41 / 54.87 / 54.40 |
| Relaxed | 13.91 / 13.71 / 13.69 | 37.59 / 32.53 / 36.47 | 47.01 / 44.78 / 45.38 | 50.51 / 49.66 / 48.77 |
| Strict batch | 14.25 / 14.07 / 14.27 | 34.19 / 35.27 / 36.00 | 46.03 / 44.90 / 45.64 | 50.38 / 49.77 / 50.67 |

Bytes moved: 10 cues is 1.4 MB read and 2.7 MB written (each file is backed
up and rewritten); 50 is 7.2 / 14.4; 100 is 16.3 / 32.5; 200 is 31.3 / 62.7.

### Projected whole save

Measured per-cue overhead plus the measured median write time:

| Cues | Today | Repeated work removed | Plus batched writes |
|---:|---:|---:|---:|
| 10 | 17.9 s | 15.5 s | 13.8 s |
| 50 | 49.8 s | 37.5 s | 35.5 s |
| 100 | 70.7 s | 46.1 s | 45.9 s |
| 200 | 104.6 s | 55.4 s | 50.7 s |

## Round 2, same day: the cost is different per format

The stray-cue round measured the rekordbox and OneLibrary paths. Sync,
Clean Up, Duplicates, Local Cue restore and Add Cue all go through the same
three cue writers, so the obvious question is whether the same answer
applies. It does not: **the right fix differs per format**, and the
scratch-copy mechanism the project already has helps exactly one of them.

`tools/engine_write_bench.cpp`, 50 tracks from RV2's own Engine Library,
copied to a scratch directory on the stick and to a ramdisk:

| One Engine cue write | Per item |
|---|---:|
| Today, database reopened per item, on the stick | 151.0 ms |
| Today, database reopened per item, on a ramdisk | 0.8 ms |
| One handle held, one `track::update(snapshot)` per item | 0.4 ms |

Put beside the OneLibrary figures from round 1, the three formats want
three different things:

| Format | Per item | Where the time goes | Fixed by |
|---|---:|---|---|
| Engine | 151 ms | almost entirely device I/O | a scratch copy: 151 ms to 0.8 ms |
| OneLibrary | 235 ms | almost entirely CPU (two PBKDF2 key derivations per call) | holding the connection: 235 ms to 4.7 ms. A scratch copy does **not** help |
| rekordbox | 15 ms + a whole-file ANLZ rewrite | two `export.pdb` parses, then per-track file writes | an index built once per save; the per-track writes are irreducible |

Two consequences worth carrying forward:

- **Sync's Engine target is already fast** and nobody noticed. It goes
  through `FormatWriteSession`, which scratch-copies `m.db` once the item
  count clears its threshold, so those writes land on a ramdisk at 0.8 ms
  each. The stray-cue path does not use `FormatWriteSession` at all, so its
  Engine writes pay the full 151 ms.
- **A scratch copy is not a general speed fix.** It converts device I/O into
  RAM I/O, so it does nothing at all for a cost that is CPU. That is exactly
  the OneLibrary case, and it is why "put it on a ramdisk" was the wrong
  instinct there.

### A proposed optimization the benchmark rejected

Collapsing the Engine writer's three auto-commit updates into one
`track::update(snapshot)` looked free: the library creator already uses that
shape, and it measured faster. It cannot be used. On RV2's real Engine
Library, `track::snapshot()` **throws for 30 of 50 tracks** ("Track data blob
doesn't have expected decompressed length"), the same libdjinterop decoder
quirk the cue writer already works around for `sample_rate()`. The current
three-setter shape writes all 50 without complaint.

Round-tripping a whole snapshot means round-tripping every field, including
ones this library cannot decode. Keep the targeted setters. This is the
clearest argument in this file for measuring against a real library rather
than a fixture: a synthetic Engine database created by `create_database()`
has none of these tracks and would have passed.

## What came out of it

**Removing repeated work halves the save. Changing the write strategy is
worth under 9% at the largest size and nothing at all below it, and costs
either a safety guarantee or a new abstraction.** Batching flushes was
the idea we started with; it is the smaller prize and the only one that
trades away either a safety guarantee or a new abstraction.

Rules of thumb for the next round:

- **Suspect per-item CPU before per-item I/O.** The single largest cost here
  was a key derivation, not a disk write. Anything that opens an encrypted
  database, parses a whole file, or checksums a whole file *per item* is a
  candidate worth more than any write-strategy change.
- **Count the opens.** Reopening a database per item is the recurring shape
  of this bug in this codebase. It has now been found in the Engine cue
  writer, the OneLibrary cue writer, and the analysis-path lookup, and it was
  the documented reason the Engine library creator builds in a scratch
  directory (`libdjinterop_engine_library_creator.cpp`).
- **Resolve once per save, not once per item.** A save knows its whole work
  set up front. Indexes, connections and writer objects belong in
  `SaveContext::shared`.
- **Do not batch durability without a measurement that justifies it.** The
  per-file flush is the strongest guarantee available and, at these sizes, it
  is not what costs the time.
- **Scratch copies are for one big file, not many small ones.**
  `FormatWriteSession` already handles the one-database case well. The
  per-track analysis files are a different shape and did not benefit.

## Platform notes

The findings above are Linux. Anything batched diverges elsewhere:

- **Windows** has no user-mode filesystem sync. Flushing a volume needs
  administrator rights, so a batch degrades to one flush per file in two
  ordered passes: the ordering guarantee survives, the flush saving does not.
  A batched rename pass must also retry on sharing violations, the way
  `compact_stick_backup.cpp` already does with `MoveFileEx` and backoff.
  Note that `durable_file_write.cpp` does not apply `longPathSafe`, so it
  cannot reach a path past `MAX_PATH`.
- **macOS** is not a build target: the non-Windows branch of the build is
  Linux-specific. If it ever becomes one, batching pays off *more* there,
  because `F_FULLFSYNC` (which `writeFileDurably` already uses on Apple,
  since plain `fsync` does not reach the drive) is markedly more expensive
  than a Linux `fsync`.

Benchmarks themselves are currently Linux-only: they depend on `syncfs`,
`posix_fadvise` and `udisksctl`. That is a gap to close, not a fact to live
with. The right shape is a C++ driver built against this project's own
platform adapters (`createRemovableMediaMounter()`,
`createRemovableMediaLocator()` in `src/infrastructure/media/`), so the same
program unmounts and remounts a stick on both platforms, rather than a shell
script full of `lsblk`, `findmnt` and `udisksctl` plus a PowerShell twin of
it. Note that Windows numbers will not be comparable to these regardless:
there is no user-mode filesystem sync there, so a batch degrades to per-file
flushes.

**No test in this project is a human checklist.** It is a one-man project;
anything that needs a person to follow steps will not get run. A test is
either an automated script or a C++ program, triggered manually if need be
but never performed manually.

## Adding a round

Keep this file as a running log. For each round, record: the stick and its
filesystem, the case under test, the per-item breakdown, the strategy
comparison with individual runs, and what the decision was. A number without
the stick it came from is not reusable.

## Round 3, 2026-09-07: one index per save instead of two parses per item

The first fix from Step 3 of the plan, and the first one the corpus
runner's work counts could prove rather than argue for.

`findAnlzPathForTrackId()` parses the whole 1.4 MB `export.pdb` to answer
one question, and a rekordbox save asked it twice per item: once in the
change class to find the analysis file to back up, once inside
`RekordboxCueWriter` to find the same file to write. A 200-item save
therefore parsed the same database 400 times.

`infrastructure/rekordbox/anlz_path_index` builds the whole map in one
pass. The save holds one per catalog in `SaveContext::shared`, every change
class asks it instead of the database, and `RekordboxCueWriter` takes an
optional pointer to it. The single-shot callers, the command line and the
waveform reader, keep the old lookup: for one id, one pass is the cheaper
answer.

Measured by `corpus_test`, which records these per data set and fails on a
change:

| Save | Parses before | After |
|---|---:|---:|
| Add one cue | 2 | 1 |
| Remove 5 stray cues | 10 | 1 |
| Library Health repair | 2 | 1 |
| Clean Up one group | 2 | 2 |

The stray-cue row is the shape of the win: flat in the number of items
rather than linear. Extrapolated to the 201-cue save that started all of
this, 402 parses become 1.

Clean Up is unchanged because its second parse is `PdbRowWriter`'s own read
of the database it is about to rewrite, which is a different cost and needs
a different fix.

A failure is not automatic if the index cannot be built: the catalog may be
unreadable, and every caller falls back to the per-call lookup rather than
failing the save.

**Still open, in order:** hold one OneLibrary connection and key derivation
per save (the corpus counts show 1 to 5 SQLCipher opens per save today,
each paying a PBKDF2 derivation, which is the 235 ms/item cost), reuse the
Engine database handle, keep the operation log's stream open, and fix the
two O(n^2) loops in `FilesystemBackupStore`.

## Round 4, 2026-09-07: one OneLibrary connection per save

The 235 ms per item. Every SQLCipher open derives the key from a
passphrase, roughly 115 ms of CPU, and the writer opened two connections
per call: one to write through, one to read the committed result back.
Nine call sites then constructed a writer per item, so a save paid that
derivation once per item at least, often twice.

Two changes, both needed. `OneLibraryCueWriter` now holds its two
connections instead of opening them per call, and the save holds one writer
per database in `SaveContext::shared` instead of nine call sites each
building their own.

The verification keeps its own separate connection rather than re-reading
through the one that just wrote. Reading the committed result back through
a different connection is the property that check exists for; what it did
not need was a fresh key derivation each time to do it. Holding the
connections is safe because every method checks the staleness guard first
and throws if the file changed underneath, so a held connection is never
used against a database the writer no longer recognises.

Measured on the real RV2 set by `corpus_test`:

| Save | SQLCipher opens before | After |
|---|---:|---:|
| Remove 5 stray cues | 5 | 1 |
| Library Health repair | 4 | 2 |
| Clean Up one group | 4 | 2 |

Flat in the number of items rather than linear, which is the shape that
matters. The remaining 2 is the floor: one write connection and one verify
connection, opened once per save.

The first attempt at this changed nothing measurable, because holding the
connections inside a writer that is still constructed per item just moves
where the derivation happens. The counts said so immediately, which is the
argument for having them.

**Still open, in order:** reuse the Engine database handle (20 opens for 20
items today), keep the operation log's stream open, and fix the two O(n^2)
loops in `FilesystemBackupStore`.

## Round 5, 2026-09-07: one Engine handle per save

`load_database()` is a full SQLite open plus schema detection, about 151 ms
against a stick, and every method did one: a 200-item save opened the same
database 200 times. The writer holds it now, opened lazily so constructing
a writer that is never used stays free. Add Cue also shared its Engine
writer for the save, the way the OneLibrary one already is.

Measured by `corpus_test`, writing 20 cues:

| | Engine opens |
|---|---:|
| Before | 20 |
| After | 1 |

Safe to hold: libdjinterop writes through its own transactions, and nothing
else in this process writes that file while a save owns the writer.

The recorded metric changed shape for the third time, and for the same
reason each time: a per-item average rounds a 20-to-1 win down to zero. It
now records the total for the batch, which is the number that says whether
the cost is flat or linear.

**Still open:** keep the operation log's stream open, and the two O(n^2)
loops in `FilesystemBackupStore`.

## Round 6, 2026-09-07: re-measured on the stick, and it changes the plan

`tools/staged_save_bench` times the workload that started all of this:
remove a 0:00 memory cue from N tracks through the real save loop, staged
exactly as the Library Health page stages them, against files on a real
stick. It works only inside `<stick>/.seabass-savebench/` and removes it
afterwards.

Measured on RV2, the same stick as the original 155 s / 201-cue save:

| Items | Wall clock | Per item | pdb parses | SQLCipher opens | Durable writes |
|---:|---:|---:|---:|---:|---:|
| 50 | 9.9 s | 198 ms | 1 | 2 | 101 |
| 201 | 135.8 s | 676 ms | 1 | 2 | 403 |

Two things to take from this, and the second matters more than the first.

**The repeated work is gone and stays gone.** One catalog parse and two key
derivations for the whole save, at any batch size. That was 402 parses and
201 derivations for the 201-item save. The counts are flat, which is what
rounds 3 to 5 set out to do.

**And it barely moved the clock: 155 s became 136 s.** Removing per-item CPU
work bought about 12%, because the save is now bound almost entirely by
per-file durable writes. 403 of them for 201 items -- one backup and one
target per cue -- and at the ~330 ms a flushed whole-file write costs on
this stick, that is essentially the entire 136 s.

**Worse, it is superlinear.** Four times the items cost thirteen times the
wall clock, and the per-item figure triples from 198 ms to 676 ms.
*(Round 8 retracts this: the same 200-item batch measured 659 ms and 278 ms
per item in one sweep. These runs did not follow round 2's own rotate,
remount and median discipline, so run order dominated them. The rest of
this section stands; the superlinearity does not.)* Nothing
in the work counts grows, so this is not repeated work coming back. The
most likely cause is the backup directory: every file a save backs up lands
in one flat directory, exFAT scans a directory linearly to insert into it,
and this save puts 201 files into one. That would make the backup half
quadratic in the number of items, in the filesystem rather than in our
code, caused by our choice of layout.

**This reverses round 2's conclusion, with evidence.** Round 2 found the
write strategy irrelevant, within 9% across three durability options. That
was measured while per-item repeated work dominated everything. It no
longer does, so the write half is now the whole cost, which is exactly the
condition Step 4 of the plan named for revisiting batching rather than
guessing at it.

**Next, in order:**

1. Confirm the superlinearity and its cause before fixing anything. Time
   the save's two halves separately, and time the same batch with the
   backups written into per-item subdirectories instead of one flat one.
   The hypothesis is specific and cheap to falsify.
2. If it is the directory, shard the backup layout. That is a small change
   and needs no batching.
3. Only then revisit batched durability, with these numbers rather than
   the old ones.

**Caveat on this run.** The stick was unmounted while a third measurement
was in progress, so a `.seabass-savebench` directory may be left on it.
Nothing else writes there and it is safe to delete. The 50 and 201 figures
above are from runs that completed and cleaned up after themselves.

## Round 7: the flat-directory hypothesis was wrong

Round 6 guessed that the superlinearity came from every backup landing in
one flat directory that exFAT has to scan linearly to insert into. That is
wrong, and the cheapest way to find out was to take the USB link out of the
picture and keep the filesystem: the same benchmark against an exFAT
loopback image on the system SSD.

| Items | Per item, exFAT on SSD |
|---:|---:|
| 25 | 19.4 ms |
| 50 | 13.3 ms |
| 100 | 11.7 ms |
| 200 | 11.1 ms |

Flat, in fact improving per item as the fixed setup amortises. Same
filesystem, same flat backup directory, same 403 durable writes for 200
items, no bend at all. So neither exFAT's directory handling nor our
backup layout explains the curve on the stick.

That leaves the medium, and two candidates worth separating before
anything is designed around either:

- **Write-cache saturation.** Small flash devices absorb the first burst
  into a fast buffer and fall back to slower direct writes once it fills.
  A longer run would then cost more per item than a short one, which is the
  shape observed.
- **Allocation on a nearly full volume.** RV2 was at 93% before these runs
  and 96% after, and the benchmark itself copies 600 MB onto it each time.
  exFAT allocation slows as free space fragments, so a run that fills the
  medium as it goes bends for reasons unrelated to the code.

The second is the more likely of the two and the easier to rule out: run
the sweep on a stick with plenty of free space, in ascending and then
descending order of batch size. If the curve follows the batch size it is
the device; if it follows the fill level it is the volume. The benchmark
now prints free space with every run so the two cannot be confused.

**Either way the conclusion for our code is the same**, and it is the one
that matters: every work count is flat, one catalog parse and two key
derivations per save at any batch size, and the remaining cost is per-file
durable writes to a slow medium. What changes with the answer is only
whether batching is worth its complexity, and that question is still open
rather than settled either way.

## What moving backups off the stick would buy

Sebastian's suggestion, and the arithmetic supports it. Of the 403 durable
writes a 201-cue save makes, 201 are backup copies and 202 are the actual
library writes. The loopback numbers put a durable write on the system SSD
at roughly 11 ms against roughly 330 ms on the stick, so those 201 backup
writes are close to the whole of half the save's cost, and moving them to
local disk would remove essentially all of it. Expect a save to roughly
halve.

What it costs is not performance but a property the current design has:
backups live at `<stick>/.seabass-backups`, so undo travels with the stick.
Move them to the local machine and Undo Last Save only works on the machine
that performed it, a stick carried to another computer has no way back, and
the backup no longer survives the loss of that computer. For a feature
whose entire job is "you can get your library back", that is a real
trade-off and not merely a location change.

A middle option keeps both: write the backups to local disk during the
save, where they are nearly free, then copy them to the stick once at the
end, as **one archive rather than 201 files**. The stick then pays one
sequential write instead of 201 flushed ones, undo still travels with the
stick, and the project already has an archive writer and reader for exactly
this shape (`infrastructure/zip_archive_writer` and `_reader`, and the
stick-backup feature's own append-only archive with its journal).

This is a design decision rather than an optimisation, so it wants deciding
rather than assuming. Recorded here so the next round starts from it.

## Round 8: the superlinearity was not real, and I ignored our own method

The sweep, ascending then descending in one session, on RV2 after a
filesystem check and with the old backups cleared:

| Items | Ascending | Descending | Free space |
|---:|---:|---:|---:|
| 25 | 906.6 ms | 429.9 ms | 0.53 GiB |
| 50 | 253.9 ms | 270.9 ms | 0.53 GiB |
| 100 | 299.7 ms | 269.3 ms | 0.52 GiB |
| 200 | **659.1 ms** | **277.9 ms** | 0.50 GiB |

The two 200-item runs are the whole answer. Same batch size, same code,
same free space to three decimal places, one immediately after the other,
and they differ by 2.4x. Whatever that is, it is not a property of the
batch size, and round 6's superlinearity does not survive it. Neither does
the fill-level explanation: free space is identical across the pair.

Corrected numbers, then. For batches of 50 and up the per-item cost is
roughly flat at 250 to 300 ms, with one outlier at 659. Against the
original 771 ms per cue that is a real improvement of about 2.5 to 3x, not
the 12% round 6 reported from a single unlucky run. A 200-cue save on this
stick lands somewhere between 56 s and 132 s depending on nothing the code
controls.

The 25-item runs being the most expensive per item is real and expected:
the fixed cost of a save (one catalog parse, two key derivations, the write
session's setup) divided by 25 rather than 200. It is the same absolute
cost, showing up larger.

**The actual lesson is procedural and it is on me.** This document's own
round 2 established the method for measuring a stick, in its own words:
rotate the order and take the median of three, and unmount and remount
between runs, because run order otherwise dominates the result. I built a
new benchmark and ran it single-shot, in order, without remounting. Round
6's conclusion, its hypothesis, and the round 7 experiment chasing that
hypothesis all follow from that one omission. The loopback result in round
7 is still valid and still useful -- exFAT is not the problem -- but it was
answering a question that should not have been asked yet.

**Before any further conclusion about the write half:** give
`staged_save_bench` the same `--device` unmount and remount that
`stick_write_bench` already has, and the same rotate-and-median discipline.
Until then the honest summary is that the work counts are flat and provably
so, the per-item wall clock is roughly flat for realistic batch sizes, and
the save is two to three times faster than it was.

**Unchanged by any of this:** 201 of the 403 durable writes in a 201-cue
save are backups, and moving them off the stick, or batching them into one
archive written at the end, remains the largest single lever available. It
does not depend on which of these numbers is right.

## Round 9, 2026-09-08: it is the file count, not the flushing

Sebastian asked whether flushing the backup to the stick in one go -- one
write, one fsync -- would beat the many small writes plus fsync the backup
store does today. It would, by a factor of twenty, and round 1 missed it
because round 1 never tested that shape. Its three "strategies" were three
ways of writing 400 separate files; the difference between them was the
durability barrier, and the barrier is not the cost.

A throwaway rig (not committed -- see the last paragraph of this round for
where it belongs), four shapes, 64.1 MiB of payload each time (400
files of 164 KiB, the mean `.EXT`), medians of three rotated runs with an
unmount, remount and three-second settle before every timed run:

| Shape | Syncs | A3 (empty) | RV2 (95% full) |
|---|---:|---:|---:|
| A one file, 1 MiB writes, one fsync | 2 | **2.07 s** | 49.93 s |
| B one file, 160 KiB writes, one fsync | 2 | **2.07 s** | 52.72 s |
| C 400 files, fsync each (today) | 800 | 49.21 s | 56.93 s |
| D 400 files, one syncfs at the end | 1 | 45.78 s | 50.59 s |

Read the A3 column, which is the one taken on a healthy device:

- **Dropping 800 fsyncs to one buys 7%** (C to D). That is round 1's answer
  and it still holds: the barrier is not where the time goes.
- **Putting the same bytes in one file instead of 400 buys 22x** (D to B).
  That is the whole cost, and it is the shape round 1 never measured.
- **The write size inside the file does not matter** (A equals B to within
  0.01 s). So an archive can be streamed entry by entry as the save runs;
  it does not need a tmpfs mirror, a memory buffer, or a size cap.

Per durable whole-file write, C works out at 123 ms on A3 and 142 ms on
RV2, against the 136 ms round 1 measured on RV2 as half of its 272 ms
backup-and-rewrite pair. Three independent measurements agree: **a durable
whole-file replacement on a USB stick costs about 130 ms, and it is a fixed
per-file cost, not a function of size.** 400 of them is 50 s.

*(Round 12 narrows this: the 130 ms is Linux-specific. Windows measures
24.6 ms for the same operation. The claim holds across Linux devices, not
across platforms.)* That is the
write half of a 200-cue save, and it is spent on file creation, allocation
and directory-entry updates, not on flushing.

### RV2 is no longer a valid benchmark device for this

RV2 cannot show the win at all: 49.93 s for a single sequential 64 MiB
write, against 2.07 s on A3. It is 24x slower at the one thing flash is
supposed to be good at, while its **per-file** cost is within 16% of A3's.
A filesystem difference (RV2 exFAT, A3 FAT32) would show up in both
numbers; only the sequential number collapsed. The obvious suspect is that
RV2 is 95% full with 1.6 GB free, which leaves its controller no spare
blocks and puts it permanently in garbage collection.

*(Round 12 undermines this: manta's stick is 97% full and writes at 16-20
MB/s. Fullness alone does not produce a 1.4 MB/s ceiling. Treat the
suspicion as unproven and RV2 as probably just a worn device.)*

This retroactively explains round 1's flat table. All three of its
strategies were measured on a device whose ceiling was already 1.3 MB/s, so
they could not differ by much whatever they did.

**Consequence for the method:** every write-half number in rounds 1 and 6
to 8 was taken on RV2 and is a measurement of a saturated stick, not of the
code. The read-half and work-count results are unaffected -- those are
counters, which is exactly why they were made counters.

### What this changes in the plan

Step 4 of `temporal-stirring-pine` recorded that "the benchmark did NOT
support this" and kept the per-file flush. That verdict was against the
wrong three options. The one-archive shape is worth 22x on the write half
and, unlike relaxed batching, it is *safer* than today rather than riskier:
one archive written and made durable before the first live overwrite is a
strict barrier, where today the backup of item 201 lands only after items 1
to 200 have already been overwritten.

The project already has the pieces. `stick_backup/archive_journal.hpp`
writes a 24-byte journal before the first appended byte and clears it after
the write verifies, and `archive_recovery.cpp` rolls a torn archive back on
the next open. `zip64_writer` already streams entries.

The cost is on the restore side: `FilesystemBackupStore::restore()` copies
files back and would need an archive-backed record. That is a real piece of
work, not a refactor.

**Not yet measured:** the same four shapes against the real backup store
rather than a synthetic rig, and on a stick that is neither empty nor full.
Both belong in `stick_write_bench` with a fourth option, so the result
inherits the remount discipline rather than depending on a scratch script.

## Round 10, 2026-09-08: zipping the backup is the wrong default

Round 9 showed that one file beats 400. The obvious next question is
whether that one file should be compressed. It should not, on a healthy
stick -- and the reason is worth writing down, because it inverts on a slow
one and the crossover is inside the range of sticks people actually use.

400 **real** `.EXT` files from RV2 (64.9 MB; random bytes would have made
this measurement meaningless), streamed entry by entry into one file, one
fsync, medians of three rotated runs with remount and settle:

| Shape | Bytes written | A3 (32.6 MB/s) | RV2 (1.4 MB/s) |
|---|---:|---:|---:|
| 400 files, fsync each (today) | 64.9 MB | 49.07 s | 56.93 s* |
| One file, stored | 64.9 MB | **1.99 s** | 44.86 s |
| One file, deflate -1 | 46.7 MB | 2.49 s | 29.19 s |
| One file, deflate -6 | 45.5 MB | 4.12 s | **26.83 s** |

\* from round 9, which used synthetic bytes; the per-file cost does not
depend on content.

Compression alone, no stick involved: deflate -1 takes 1.02 s of CPU and
reaches 72%; deflate -6 takes 2.69 s and reaches 70%.

**ANLZ waveform data barely compresses.** Thirty percent off is a poor
return, and it is what sets the whole trade-off. Deflate -1 saves 18.2 MB
at a cost of 1.02 s, so it only pays where the stick writes slower than
about 18 MB/s; deflate -6 saves 19.4 MB for 2.69 s and needs a stick slower
than about 7 MB/s.

A3 writes at 32.6 MB/s, so compressing costs 25% (level 1) to 107% (level
6) more wall clock than just storing. RV2 writes at 1.4 MB/s, so
compressing saves 35% to 40%. Both sticks are 30 GB USB 3 devices bought
for the same purpose. **The right level is a property of the device, not of
the format**, which means a hardcoded choice is wrong for half the users
either way.

### What to build

Store, and get the 24x from the single file rather than from the codec.
`zip64_writer` is already STORE-only (`zip_format.hpp:38`), so this needs no
new compression code at all -- which is the strongest argument for it.

**Worth measuring before settling, though:** compressing on one thread while
writing on another makes the total `max(cpu, io)` instead of `cpu + io`. On
A3 that projects to 46.7 MB at 32.6 MB/s = 1.43 s of writing with 1.02 s of
compression hidden behind it, which would beat storing (1.99 s) on the fast
stick as well as on the slow one, and remove the device-dependent choice
entirely. That is arithmetic, not a measurement, and this document has been
burned by exactly that distinction before -- so it is a hypothesis to test
in `stick_write_bench`, not a design decision yet.

## Round 11, 2026-09-08: threads alone do nothing; threads plus early writeback work

Round 10 projected that compressing on worker threads while writing would
make the total `max(cpu, io)` and let deflate beat storing on a fast stick.
Tested on A3, same 400 real `.EXT` files, medians of three rotated runs with
remount and settle. The projection was right about the destination and wrong
about the route.

First attempt, worker threads only:

| Shape | Median | Bytes out |
|---|---:|---:|
| Stored, serial | 2.10 s | 64.9 MB |
| deflate -1, serial | 2.54 s | 46.7 MB |
| deflate -1, pipeline x1 | 2.50 s | 46.7 MB |
| deflate -6, serial | 4.19 s | 45.5 MB |
| deflate -6, pipeline x1 | 4.14 s | 45.5 MB |
| deflate -6, pipeline x4 | 2.20 s | 45.5 MB |

One worker bought 0.04 s. Four bought 1.99 s, but still lost to storing.
The suspicion was the GIL, so it was checked rather than assumed: zlib
compression alone scales 1.94x / 3.61x / 6.02x on 2 / 4 / 8 threads. Not the
GIL.

**The real reason: there is nothing to overlap with.** Buffered writes land
in the page cache almost free, and the entire device cost is the single
fsync at the end. So the total is `compression_wall + flush`, and adding
compressor threads only shrinks the first term. The arithmetic confirms it
to within 0.05 s on every row: serial -6 is 2.78 CPU + 1.40 flush = 4.18
against 4.19 measured; stored is 0.13 + 2.0 = 2.13 against 2.10.

Real overlap needs writeback *started* while compression is still running.
`sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE)` every 4 MB queues it
without waiting:

| Shape | Median | Bytes out |
|---|---:|---:|
| Stored | 2.06 s | 64.9 MB |
| Stored + writeback | 2.04 s | 64.9 MB |
| deflate -6 x4 | 2.20 s | 45.5 MB |
| deflate -6 x4 + writeback | 1.77 s | 45.5 MB |
| **deflate -6 x8 + writeback** | **1.59 s** | 45.5 MB |

Both halves are needed and neither works alone. Writeback does nothing for
the stored case (2.04 against 2.06) because there is no CPU to hide. Threads
did nothing without it. Together they reach 1.59 s against an overlapped
ideal of 1.40 s, so 88% of the available win, and they beat storing by 23%
while putting 30% fewer bytes on the stick. Output verified byte-identical
to the serial archive in every configuration.

### It still does not change the recommendation

Store, and here is why the faster option loses anyway:

- On a fast stick the whole prize is **0.47 s** (2.06 to 1.59) on a save
  that costs 49 s today. It buys a thread pool, a writeback policy and a
  codec to win half a second.
- `sync_file_range` is **Linux-only**, and there is no clean non-blocking
  equivalent on Windows. So the 23% is unavailable on a platform this
  project targets, while the 24x from the single file is portable.
- On a slow stick the win is real and large (round 10: 26.83 s against
  44.86 s on RV2), but it comes from writing fewer bytes, not from
  threading -- the compression term is already small next to a 31 s write.

So: single stored archive first, for 24x and no new code. If compression is
ever added, add it for slow devices, where plain serial deflate already
captures most of it.

### Windows: requested, not yet measured

Everything in rounds 9 to 11 is Linux. The Windows claims in those rounds --
that a durable write costs the same order there, that the single-file win
survives NTFS and exFAT under `FlushFileBuffers` + `MoveFileEx`, and that
`sync_file_range` has no clean non-blocking analogue -- are reasoning from
the API surface, not measurements, and are flagged here so nobody builds on
them by accident.

A run was handed to the `windows` session on manta on 2026-09-08 with a
port of the rig that drops the two shapes Windows cannot express (no
user-mode volume sync, no `sync_file_range`). The three numbers it should
settle: whether the single-file win is still roughly 24x, what one durable
whole-file write costs in ms against Linux's device-independent ~130 ms, and
which side of the deflate crossover manta's stick falls on. Update this
section with the result rather than leaving the assertions standing.

## Round 12, 2026-09-08: Windows confirms the shape, denies the magnitude

Run on manta by the `windows` session, same rig minus the two shapes
Windows cannot express. Device: `D:`, FAT32, 29.28 GB with 0.82 GB free --
97% full, and the user's real stick rather than a scratch one, cleared with
them first and read-only apart from the scratch directory. Python 3.14.7.
Same 400 real `.EXT` files, 64.9 MB.

| Shape | Median | Runs | Bytes out |
|---|---:|---|---:|
| 400 files, flush each (today) | 9.82 s | 9.45 / 11.14 / 9.82 | 64.9 MB |
| **One file, stored** | **3.91 s** | 3.91 / 4.46 / 3.15 | 64.9 MB |
| One file, deflate -1 | 5.49 s | 5.60 / 5.10 / 5.49 | 46.7 MB |
| One file, deflate -6 | 9.09 s | 9.09 / 9.11 / 8.96 | 45.5 MB |

Compression alone: deflate -1 2.22 s (29 MB/s), deflate -6 5.77 s (11 MB/s),
reaching the same 72% and 70% as on Linux. The ratios are a property of ANLZ
data and travel; the CPU rates are manta's, roughly half this machine's.

**Two of the three answers hold, one does not.**

*The single-file win is real but far smaller: 2.5x, not 24x.* Still worth
having, still the right design, and now confirmed on the platform where
`sync_file_range` was never available anyway.

*Deflate loses to stored here too*, and for the predicted reason: this stick
writes at 16-20 MB/s stored, above the ~18 MB/s deflate -1 break-even. Two
platforms, two filesystems, three devices, same verdict. **Store.**

*But the ~130 ms durable write is Linux-specific, not device-independent.*
Windows costs 24.6 ms per file gross, 14.8 ms net of the sequential floor,
against 123 ms on an empty Linux stick and 142 ms on a full one. Round 9
called that constant device-independent on the strength of two Linux
devices; it is platform-dependent, and the wording there is now wrong.

**Retracted in round 13 -- round 9's own table refutes it.** The likely
mechanism, and it is a hypothesis, not a measurement: on Linux
`fsync` to removable media issues a real cache-flush round trip to the
device per call, and this rig makes two per file (the file and its
directory). Windows mounts removable devices under the "quick removal"
policy with write caching disabled, so the writes are already write-through
and `FlushFileBuffers` has little left to do -- and the directory flush has
no equivalent and is not needed. Flush count alone does not close the gap
(2 x 24.6 is 49 ms, not 123), so the per-flush cost has to differ too.

### This also undermines round 9's explanation of RV2

Round 9 blamed RV2's 1.4 MB/s sequential write on it being 95% full.
Manta's stick is **97% full and writes at 16-20 MB/s**. Different OS,
filesystem and device, so this is not conclusive -- but fullness alone
clearly does not produce a 1.4 MB/s ceiling, and RV2 is better explained as
simply a worn or poor device. Round 9's suspicion should be read as
unproven.

### Still open

Manta had only the one, very full device, so there is no Windows equivalent
of the empty-stick comparison and no way to separate "Windows is cheap per
file" from "this particular stick is". A follow-up asking for a D:-read /
C:-write split has been sent, which isolates the syscall cost from the
medium, along with the write-caching policy of the device, which would
confirm or kill the mechanism above.

## Round 13, 2026-09-08: the per-file cost is real everywhere, and my mechanism was wrong

Follow-up on manta, same 400 real `.EXT` files read from `D:`, written to
`C:` (internal disk) instead of the stick:

| Shape | C: internal | D: stick | Linux A3 |
|---|---:|---:|---:|
| 400 files, flush each | 1.48 s | 9.82 s | 49.21 s |
| One file, stored | 0.40 s | 3.91 s | 2.06 s |
| Ratio | 3.7x | 2.5x | 24x |

**The shapes do not converge off the stick.** The ratio widens on the faster
medium, which settles the question that run was for: the per-file cost is a
genuine `CreateFile` + `WriteFile` + `FlushFileBuffers` + `MoveFileEx` path
cost, not stick latency. Net of the single-write floor it is 2.7 ms/file on
C:, 14.8 ms/file on D:, and about 118 ms/file on A3 under Linux.

### Retracting the quick-removal explanation

Round 12 proposed that Windows is cheaper per file because removable devices
default to Quick Removal with write caching off, so `FlushFileBuffers` has
nothing to flush, while Linux `fsync` pays a device cache-flush round trip
per call.

**Round 9's own table already refuted this and I did not notice.** On A3,
400 files with an `fsync` each cost 49.21 s and 400 files with a *single*
`syncfs` at the end cost 45.78 s. Removing 799 of 800 flushes bought 7%. If
flush round trips were the Linux cost, that number would have collapsed.
They are not, so the asymmetry with Windows is not explained by having
fewer of them.

The manta session could not read the effective policy either
(`Get-StorageAdvancedProperty` returns ErrorCode 40001 for this device
class; no `UserRemovalPolicy` override exists in the registry, which is
consistent with the default but does not confirm it) and correctly declined
to claim otherwise. That check is now moot -- the hypothesis it was meant to
test is dead on other evidence.

**What is left is a description, not a mechanism:** writing 400 small files
into a directory costs roughly 118 ms each on Linux's exFAT-over-USB path
and roughly 15 ms each on Windows' FAT32-over-USB path, and neither figure
is dominated by the durability barrier. Somewhere in allocation, directory
entries and FAT chain updates, Linux is paying about eight times what
Windows pays for the same logical work. That is worth knowing and is not
worth guessing about further here; it changes no decision below.

### Nothing about the decision changes

Three media, two platforms, two filesystems:

- One stored archive beats 400 durable files everywhere: 24x, 3.7x, 2.5x.
- Compression loses everywhere a healthy device is involved, and loses worst
  where the medium is fastest -- on C: deflate -1 costs 2.63 s against 0.40 s
  stored, exactly as the ~18 MB/s break-even predicts for a 160 MB/s disk.
- `zip64_writer` is already STORE-only, so this needs no codec.

Build the single stored archive. The remaining work is on the restore side,
where `FilesystemBackupStore::restore()` copies files back and needs an
archive-backed record.

### Gap: threading was never tested on Windows

Rounds 12 and 13 have no threaded shape. When the rig was ported, the
worker-thread shapes were dropped along with the early-writeback ones --
but only `sync_file_range` is Linux-only; threads are not. That was an
over-correction and it left the question unanswered rather than answered
negatively.

It could plausibly come out differently there. Round 11 found that threads
alone buy nothing on Linux because buffered writes are free and the entire
device cost sits in the closing fsync, leaving no in-flight I/O to hide
compression behind. If Windows removable media are genuinely write-through,
the I/O is spread through the write loop instead and threads would pay with
no extra syscall. That reasoning leans on the same quick-removal story round
13 retracted, so it is a reason to measure, not a prediction.

Arithmetic on manta's round 12 numbers, pending the real thing: deflate -6
costs 5.77 s of CPU and 45.5 MB at D:'s stored rate is about 2.74 s, so
perfect overlap on 8 threads projects to ~2.7 s against 3.91 s stored --
a possible win on the stick, and still a clear loss on C: (~0.90 s against
0.40 s). Same crossover as Linux, one medium further along it.

A run has been requested on both `D:` and `C:`. Even if it wins on the
stick, the prize is about a second against an option that needs no codec
and no thread pool, so it is unlikely to move the recommendation -- but
"not measured" is not the same as "does not help", and the doc should not
imply the second when it means the first.

## Round 14, 2026-09-08: threading does pay on Windows

Run on manta, both destinations. Same 400 files, 4 cores.

| Shape | D: stick | C: internal | Bytes out |
|---|---:|---:|---:|
| Stored | 14.53 s | 0.40 s | 64.9 MB |
| deflate -6 serial | 19.29 s | 5.96 s | 45.5 MB |
| deflate -6 threads x4 | 12.54 s | 2.68 s | 45.5 MB |
| **deflate -6 threads x8** | **11.54 s** | 2.41 s | 45.5 MB |

Threaded output byte-identical to serial in every configuration.

**On the stick, threaded deflate beats storing.** On the internal disk it
loses badly. That is the same crossover as Linux, and Windows reaches it
without `sync_file_range`: the CPU column shows real overlap on D: (wall
time falls from 19.29 to 11.54 while CPU stays flat at 6.1-6.6 s) and none
on C: (CPU flat at 6.8-7.1 s regardless of worker count, because there is no
I/O wait to hide behind). Round 11 predicted exactly this if Windows
removable media are write-through. That is consistent with the buffering
half of the quick-removal story; it says nothing about the flush-count half,
which round 13 retracted for separate reasons.

**Magnitude caveat, raised by the manta session and worth keeping.** `D:`
had degraded roughly 4x by this run -- stored measured 3.91 s earlier in the
day and 14.53 s here, on the same stick with *more* free space than before.
In between it absorbed about 26 GB of write traffic. So the direction of the
threading result is sound (all four shapes share the degraded state) but the
absolute figures are not comparable to round 12's.

That degradation is itself the useful finding. A stick drops into a slow
regime after heavy write traffic and climbs out later -- which is precisely
the state a DJ stick is in immediately after a library sync, which is
precisely when a Save runs. **The slow regime is not the exceptional case;
it is the normal one for this application.** It also finishes off round 9's
fullness theory: free space went up while throughput went down 4x.

## Round 15, 2026-09-08: space is the constraint, and it changes the answer

Sebastian's point, which none of rounds 9 to 14 costed: a cue backup is left
on the stick permanently, so its size is not a transient cost like seconds
are. Measured cluster sizes are 32 KB on RV2 (exFAT) and 16 KB on A3
(FAT32). On-disk footprint of one 400-file backup:

| Shape | RV2, 32 KB clusters | A3, 16 KB clusters |
|---|---:|---:|
| 400 separate files | 71.50 MB | 68.22 MB |
| One stored archive | 64.88 MB | 64.88 MB |
| One deflate -6 archive | **45.55 MB** | **45.55 MB** |

The archive alone recovers 6.62 MB of slack on RV2 (400 files, each rounded
up to a 32 KB boundary). Compression recovers a further 19.33 MB. Together
that is 36% of the footprint, about 26 MB per save, or roughly three tracks.

**And backups accumulate.** `prune()` exists but nothing calls it
automatically -- only `cli/main.cpp:685` with an explicit keep count, and
the Backups page deleting one record at a time. So this is permanent,
growing consumption on devices that are 95% and 97% full in the only two
real samples available.

### Revised recommendation: deflate the archive, at level 1

Rounds 10 to 12 said store, on the strength of wall clock alone. Adding the
space axis and the threading result reverses it.

*Level 1, not 6.* Level 1 reaches 46.7 MB, level 6 reaches 45.5 MB. That
last 1.2 MB costs 2.6x the CPU (1.02 s against 2.69 s here, 2.22 s against
5.77 s on manta). Level 1 captures 93% of the space saving for 40% of the
work.

*And the time cost mostly vanishes on the media that actually hold backups:*

| Device | Stored | Best deflate | Winner |
|---|---:|---:|---|
| A3, healthy Linux stick | 2.06 s | 1.59 s (-6 x8 + writeback) | deflate |
| D:, degraded Windows stick | 14.53 s | 11.54 s (-6 x8) | deflate |
| RV2, slow Linux stick | 44.86 s | 26.83 s (-6 serial) | deflate |
| C:, internal disk | 0.40 s | 2.41 s | stored |

Deflate wins on every stick measured and loses only on an internal disk,
which is not where backups live. It always saves about 30% of permanent
space. The earlier verdict came from measuring one axis on one healthy
device and calling it the answer.

*Costs this adds:* a thread pool, and deflate support in `zip64_writer`,
which is STORE-only today. The `sync_file_range` kick stays a Linux-only
refinement; Windows gets its overlap for free.

**Not measured, and it should be before this is built:** threaded deflate
*level 1* anywhere -- every threaded number above is level 6 -- and any
threaded run on a Windows stick in a healthy state.

## Round 16, 2026-09-08: level 1 threaded, and what the spread will and will not support

Manta added level-1 shapes alongside the level-6 ones in a single run, so
all seven are comparable under one device state. Pooling both Windows
sessions on the degraded `D:` (all runs, not just the reported medians):

| Shape | n | min | median | max | spread |
|---|---:|---:|---:|---:|---:|
| Stored | 6 | 10.41 | 14.09 | 18.39 | 57% |
| deflate -6 serial | 6 | 13.50 | 15.75 | 19.88 | 41% |
| deflate -6 x4 | 6 | 10.19 | 11.69 | 12.79 | 22% |
| deflate -6 x8 | 6 | 9.03 | 11.28 | 12.42 | 30% |
| deflate -1 serial | 3 | 12.26 | 13.06 | 16.04 | 29% |
| deflate -1 x4 | 3 | 6.99 | 8.49 | 11.91 | 58% |
| deflate -1 x8 | 3 | 10.61 | 10.77 | 13.15 | 24% |

**What survives:** threaded compression beats storing. Stored sits at 14.09
across six runs, every threaded shape sits between 8.5 and 11.7, and the
ranges barely overlap. Threading also clearly beats serial at both levels.

**What does not survive:** the ranking among the threaded shapes. With
spreads of 22-58% on three to six samples, level-1 x4 at 8.49 cannot be
called faster than level-6 x8 at 11.28, and level-1 x8 landing *slower*
than level-1 x4 while level-6 x8 lands *faster* than level-6 x4 is not a
mechanism, it is noise on a 4-core box. The manta session flagged the
variance and was right to; the "fastest shape in the table" reading is one
step further than the data goes.

**The level-1 decision does not need that ranking anyway.** It rests on two
low-variance measurements instead:

- *Space:* 46.7 MB against 45.5 MB. Level 6 buys 1.2 MB out of a 26 MB
  saving.
- *CPU:* 2.3-2.8 s against 6.2-6.6 s, consistent across every run in the
  table. In a GUI application doing this during a save, that is the number
  that shows up as responsiveness, and it is two and a half times better.

Wall clock is at worst a wash between the two levels. So: **level 1,
threaded.** The recommendation from round 15 stands and now rests on
measurements whose error bars do not swallow it.

### The device did not recover

Before this run, `D:` measured 12.45 s for a single stored write, against
3.83-3.91 s earlier in the day and 12.05-18.39 s while degraded. Several
minutes of idle time did not bring it back, and its *variance* got worse as
well as its median. RV2 here shows the same permanence.

That is worth more than the benchmark it interrupted. **These devices enter
a slow regime after heavy write traffic and do not climb out on a timescale
a user would notice.** A DJ stick that has just had a library synced onto it
is in that state for the rest of the session, which is exactly when saves
happen. Round 14 called the slow regime the normal operating condition; this
says it is also a sticky one.

Gap 2 -- threaded deflate on a *healthy* Windows stick -- stays open, and
waiting on this device is not the way to close it. It does not gate the
decision: the space case is independent of it, and on Linux the healthy-stick
case is already measured (A3, threaded deflate 1.59 s against stored 2.06 s).

## Why the sticks are slow, and why "the normal case" is the slow one

Rounds 14 and 16 established that a stick drops into a slow regime after
heavy write traffic and does not climb out. The cause is checkable and was
checked: **neither stick supports TRIM.**

```
$ lsblk -D -o NAME,DISC-GRAN,DISC-MAX /dev/sda /dev/sdb
NAME   DISC-GRAN DISC-MAX
sda           0B       0B
sdb           0B       0B
$ fstrim /media/sebas/RV2
fstrim: FITRIM ioctl failed: Operation not permitted
```

`discard_max_bytes` is 0 on both, so the block layer knows the devices
advertise no discard capability. USB mass storage over the BOT protocol has
no TRIM path at all.

**The filesystem can free space; the controller is never told.** That single
fact explains the observation that made no sense otherwise -- manta's stick
got 4x slower while its *free space went up*. Deleting 1.83 GB returned
clusters to the FAT and told the flash translation layer nothing. Every LBA
ever written still looks like live data to the controller.

The rest is standard flash behaviour rather than anything measured here, but
it follows directly. NAND is programmed a page at a time and erased only a
block at a time, blocks being a thousand times larger than the pages. A
controller stays fast by keeping a pool of pre-erased blocks to write into.
When the pool runs low it must garbage-collect: choose a block, copy the
still-valid pages out, erase it -- and erase is the slow operation, now on
the critical path of every write. Cheap sticks additionally absorb incoming
data into a small pseudo-SLC region and fold it down to TLC afterwards, which
is why a fresh device looks fast (A3: 32.5 MB/s) and the same class of device
looks slow once that region is full and the folding is backlogged (manta:
16-20 MB/s falling to ~5).

Without TRIM the pool is never replenished by deletion, only by internal
garbage collection, which needs idle time and has little it can legitimately
reclaim. Hence "did not recover after several minutes".

**This also settles round 9 properly.** Fullness was the wrong variable, but
it was pointing at the right one. What matters is how much of the device the
FTL believes is live, which is a function of everything ever written to it,
not of what the filesystem currently uses. A 97%-full stick written once
sequentially can be fast; a half-empty stick that has had hundreds of
gigabytes written and deleted over its life looks entirely full to its
controller and behaves accordingly.

So A3 at 32.5 MB/s is the anomaly, not the norm. It was empty and barely
used. **A DJ's working stick, months into its life with no TRIM ever issued,
lives permanently in the slow regime**, and that is the device this feature
runs on.

### What follows for the design

- It confirms the archive. Few large contiguous writes are exactly what a
  controller with no free blocks handles least badly; four hundred scattered
  164 KB writes are the worst case.
- **It strengthens compression more than the timing did.** On a device that
  never receives TRIM, every byte ever written is permanent pressure on the
  FTL, and deleting a backup later gives the controller nothing back. Writing
  26 MB less per save is not merely 26 MB of visible free space; it is 26 MB
  the device never has to carry.
- Unrelated and still unexplained: the per-small-file cost asymmetry between
  Linux (~118 ms) and Windows (~15 ms). That is a host-side path difference,
  not a device one -- both platforms were measured against the same class of
  hardware -- and round 13 retracted the one explanation offered for it.

## Round 17, 2026-09-10: round 4's fix had a hole, and it was the adapter

Round 4 held one OneLibrary writer per database in `SaveContext::shared`
and measured the derivation cost flat in the item count. It also wrote
down, about its own first attempt, that "holding the connections inside a
writer that is still constructed per item just moves where the derivation
happens". `OneLibraryCueWriterAdapter` was doing exactly that the whole
time, in the branch round 4 did not measure: `writeHotCues()` built a
fresh `OneLibraryCueWriter` on every call, and Restore Metadata keyed its
shared writer per `sourceId` on top of that, so each track got its own
adapter and its own two opens.

Counted rather than argued, by `tests/restore_metadata_change_test.cpp`
case 6, on a three-track OneLibrary restore:

| Tracks in one save | SQLCipher opens before | After |
|---:|---:|---:|
| 1 | 3 | 2 |
| 3 | 7 | 2 |

Two opens per track, plus one for the annotation writer. At ~115 ms of
PBKDF2 per open a four-hundred-track restore paid about ninety seconds of
pure key derivation, and no existing test would have seen it: the corpus
matrix in `docs/real-data-testing.md` has no row for
`RestoreMetadataChange`, and every save it does have is one item deep in
this format.

The fix is round 4's own, applied where it was missing. The adapter holds
its writer instead of building one per call; it takes a track's path
through `notePath()` as each change applies, so the save's key need not
carry a `sourceId`; and the rating and comment go through that same held
writer rather than opening a second one. Flat at two -- the write
connection and the verify connection -- which is the floor round 4 set.

The assertion is on the shape, not the number: one track and three tracks
must cost the same. A constant that happens to be right today is not what
went wrong here; linear growth nobody was counting is.

**Still open, and unchanged by this:** `MergeCuesChange` keys its writer
per track the same way (the `2N / 1` row for Local cue restore in the
corpus matrix), and the Engine handle and the operation log's stream are
still reopened. Local cue restore is the deprecated ancestor of Metadata
Backup and may be removed before it is worth optimising -- see
`docs/metadata-backup-plan.md`.
