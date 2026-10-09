<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Sync after Rekordbox Export

Bring the Engine library in line with the latest changes after a
Rekordbox export. Issue #73. Plan written 2026-10-09 against master at
dcb18a17, every claim checked against the source that day; the file and
line references are to that tree.

## The problem

A DJ adds tracks and edits playlists in rekordbox, exports to the stick,
and the Engine side of the same stick stays as it was. Nothing closes
that gap:

- rekordbox's export regenerates `export.pdb`, `exportLibrary.db` and the
  ANLZ files of the exported tracks from master.db. New tracks and
  playlists exist on the rekordbox side only.
- The player does not catch up. Engine OS 5.0.4's "update the Rekordbox
  library" prompt adds nothing: measured on the Prime 4 on 2026-10-03
  (`tools/engine-import-probe.sh`, round 2: rekordbox rows with cues and
  metadata but no Engine row; 21 of 21 cases KEPT, the counter levelled,
  `project/evidence/engine-import-probe-2026-10-03-round2`).
- Seabass does not catch up. Sync Cue Points matches tracks both sides
  have and ignores the rest. Library Health's playlist alignment adds a
  track to Engine's copy of a playlist only if Engine already lists the
  file (`src/domain/playlist_sync.hpp`, by design). Create Engine
  Library builds from scratch and its card is hidden once an Engine
  library exists. Compare Playlists shows the gap and closes nothing.
- The stick card already notices. The "Sync Needed" badge fires when
  `export.pdb`'s sequence differs from
  `Information.lastRekordBoxLibraryImportReadCounter`
  (`src/infrastructure/engine/engine_import_state.hpp`), which is
  "rekordbox wrote this stick since Engine last saw it". It leads to Sync
  Cue Points, the wrong page for a new track.

Sebastian's rule for the feature, 2026-10-09: sync everything rekordbox
updated, tracks added and removed, playlists created, deleted and
renamed, playlist membership, metadata. Show all of it, checked by
default. Except where applying rekordbox's state would overwrite
something Engine has that rekordbox only lacks because its export
regenerated the stick from master.db, above all cues Seabass had synced
from Engine earlier: those go the other way, back onto the rekordbox
side, also checked. Anything whose provenance is unknown is a conflict
the user decides, with the reason said (the "unsure means conflict"
rule, 2026-10-04).

## The question everything hangs on: what did rekordbox change?

A state diff between the two catalogs says that they differ, not who
changed what. The feature needs direction per item, and neither format
records it:

- Seabass stores nothing usable today. `application::catalogDigest`
  hashes tracks on demand and is never persisted. `LibraryFingerprint`
  is a bottom-k sample that estimates containment and cannot list
  items. `LocalCueStore` keeps only tracks with cues and no playlists.
  `MetadataStore` merges across sticks, later edit wins, and cannot say
  what one stick's rekordbox looked like at a point in time. The catalog
  cache is in memory only.
- Engine rows carry `pdbImportKey`: on the anonymized fixture 1469 of
  1564 rows hold the import counter's value and 95 hold 0. So it says
  "this row came from a player import", nothing more. libdjinterop's
  `create_track` writes 0. `originDatabaseUuid` and `originTrackId` are
  the library's own. `dateAdded` and `lastEditTime` are clocks, and this
  project lets no clock decide a sync (the `SyncPlan` comment in
  `sync_planning.hpp`).
- The rekordbox side has the pdb header sequence, which moves on every
  export and on every Seabass pdb write: a cheap "something moved"
  trigger, useless for "what". Row `date_added` is the collection add
  date. ANLZ mtimes are FAT clocks that Seabass's own writes touch too.
- The counter is no longer a content signal either. Since 2026-10-02
  every Seabass save with both catalogs levels it
  (`src/gui/edit/save_loop.cpp`, `keepImportLevel` and
  `settleImportLevel`, called at lines 253 and 268), so after any save the
  badge goes quiet while Engine has not caught up with a single track.

So a recorded base is required. **Decision: Seabass records a rekordbox
baseline at every point where it knows the two sides are level, and
plans by a per-item three-way merge of baseline, rekordbox now and
Engine now.**

### The three-way rule

For every item (a track's presence, a playlist's existence and name, a
membership, a rating, a comment, each cue), with B the baseline, R the
rekordbox side now and E the Engine side now:

| case | meaning | proposal |
|---|---|---|
| R equals E | level | nothing |
| B equals E, R differs | rekordbox changed it | apply to Engine, checked |
| B equals E, R lacks it, and B says Seabass put it there | the export regenerated the file and dropped what Seabass had written | restore onto rekordbox, checked |
| B equals R, E differs | Engine changed it (deck, Engine DJ) | listed as "Engine's own, kept"; cues stay Sync Cue Points' business |
| B, R and E all differ | both changed | conflict, unchecked, reason given |
| no B for this item | first use, or a stick never saved since this shipped | the fallback below |

The base only has to be right per item: any earlier rekordbox state is a
valid base for an item where it equals one side. That is what makes it
robust, and it is why a full stick backup's `export.pdb` can serve as an
emergency base later (not in v1).

### Provenance of cues and ratings on the rekordbox side

The restore case needs to know that a cue or rating on the rekordbox
side was written by Seabass (synced from Engine, or added in Seabass's
cue editor), not exported by rekordbox. Each baseline cue and rating
carries an origin: rekordbox, Seabass, or unknown. Seabass learns the
Seabass origin from its own writes:

- in this page's own save, exactly;
- in every other save that writes rekordbox or OneLibrary cues or
  ratings (`SyncPlanChange` towards rekordbox or OneLibrary,
  `RestoreMetadataChange` towards rekordbox, `AddCueChange` on
  rekordbox), through one new optional virtual on `PendingChange`
  (`rekordboxWrites()`, step 9 below), gathered by the save loop.

### Where the baseline lives

On the stick: `<stick>/Seabass/rekordbox-baseline.tsv.gz`, a new
`paths::stickRekordboxBaseline(stickRoot)` beside `stickMetadataCache`
(`src/infrastructure/paths/seabass_paths.hpp`). Not in the caches
directory, which is documented as safe to delete. On the stick rather
than on the computer because:

1. Undo. The file is a backup target of the save, so Undo Last Save
   rolls the baseline back with the catalogs. A file on the computer
   would silently disagree after an undo.
2. Restore and Clone carry it with the library it describes.
3. Two computers keep one truth.
4. `SaveContext` knows the stick root, not the library id.

A mirror under `localMetadataDir()` can follow if wanted; v1 does not
need it.

Format: TSV, deflated with the existing zlib compressor, escaping and
the closing `#sha256` line as in `BackupManifest`
(`src/application/backup_manifest.hpp`):

```
seabass-rekordbox-baseline <TAB> 1 <TAB> pdbSequence <TAB> engineInformationUuid <TAB> recordedAtUnix <TAB> writer
p  pdbPlaylistId  parentId  isFolder  path("Folder/List")
t  pathKey  stickRelativePath  pdbTrackId  analysisFile  rating|-  ratingOrigin(r|s|?)  comment  bpm  durationMs
c  kind  pad  posMs  isLoop  endMs  color  origin(r|s|?)        (the cues of the preceding t)
m  pdbPlaylistId  position  pathKey                           (ordered membership)
x  itemKey  rStateHash                                        (declined by the user)
#sha256  <hex>
```

`pathKey` is `application::normalizedPathKey` of the stick-relative
path, the same signal `matchTracks` treats as decisive within one stick.
Playlist ids come from `rekordboxPlaylistIdsByPath`
(`kaitai_rekordbox_reader.hpp`): they detect renames (same id, new path)
and include empty playlists, which `Track::playlists` cannot.

Size: measured on the anonymized fixture (1564 tracks, 2074 playlist
entries, 33 playlists) 379 KB raw, 34 KB deflated; placeholder text
compresses unrealistically well, so expect 0.5 to 0.8 MB raw and 100 to
250 KB deflated on a real stick. Negligible on a 97 percent full stick;
parsing is milliseconds.

### When the baseline is written

Per item, never wholesale:

1. **This page's save.** Every item applied or already level advances to
   R now. Declined items and unresolved conflicts keep their old B so
   they come back next time, and each declined item gets an `x` line so
   the badge does not nag about it until rekordbox changes that item
   again. The pdb sequence is re-read after the commit, because a
   restore may have written `export.pdb`.
2. **Any other save that wrote the rekordbox or OneLibrary side while
   the baseline was current** (its sequence equal to the pdb sequence
   read at save start): the touched tracks are refreshed from a Tracks
   stage read of `export.pdb` (about 0.1 s) and the ANLZ files the save
   backed up, the written cues and ratings are marked Seabass origin,
   and the sequence advances to the post-save value. If the baseline was
   not current (rekordbox exported in between), only the origin ledger
   is merged in and the base does not advance, since advancing it would
   hide the export's changes.
3. **Never on a read path.** Seabass reads do not write to the stick.

### The fallback without a baseline

First use, or a stick never saved since this shipped:

- Track in R, not in E, file present, not streaming: add to Engine,
  checked. Reason: no earlier record of this stick; rekordbox lists it
  and Engine does not. Additive and undoable.
- Track in E, not in R: `pdbImportKey` not 0 means the player imported
  it from rekordbox and rekordbox no longer lists it, and nothing
  records whether it was removed there: conflict. `pdbImportKey` 0 is
  Engine's own: listed, kept.
- Playlist in R only: create, checked. Playlist in E only: Engine's own,
  kept.
- Membership: rekordbox-to-Engine additions checked; Engine-only
  members a conflict, since removing them is destructive.
- Ratings and comments that differ: conflict.
- Cues: exactly what `SyncPlanner::plan` decides today, so the fallback
  is Sync Cue Points' behaviour.

**Decision:** additive, unattributed items are checked; destructive ones
are conflicts. Reading "unsure means conflict" strictly would make every
addition a conflict on first use, which turns the page into a wall of
questions on the one run where it is most useful. Sebastian may want the
strict reading; it is one flag per section.

**Decision (review of step 2, 2026-10-09):** in the fallback, an empty
rekordbox value against a value Engine has (no comment while Engine has
one, unrated while Engine is rated) is Engine's own, kept, not a
conflict: removing a value is destructive and unattributed, so the first
run must not offer it. Two differing non-empty values stay a conflict.

**Decision (review of step 10, 2026-10-09):** a conflict left unanswered
on a baseline-less run is recorded as the rekordbox side stands, so on the
next run the item reads as Engine's own and is kept, not asked again. No
later information could answer the question, nothing is written, and a
page that repeats a thousand unanswerable questions is worse than one
that says, once, that undecided items count as Engine's own from now on.
The first-run intro says exactly that.

**Decision:** rows Seabass creates keep `pdbImportKey` 0. Stamping the
pdb sequence would make them look like player imports, which is a lie
with unmeasured player semantics; the baseline is the provenance.

## The page

Name: **Sync after Rekordbox Export**. Card in the Sync group beside Sync
Cue Points, subtitle "Bring the Engine library in line with the latest
changes after a Rekordbox export", shown when the stick has both
catalogs and is writable, read-only and lock handling copied from the
Sync Cue Points card. Behind `experimentalFeaturesEnabled` until the
hardware rounds below pass, the way Create Engine Library is.

The page is a proposal first, then one save, as Sync Cue Points is: an
intro line saying what it was compared against ("Compared with how this
stick looked when Seabass last saved it, export 15132" or "No earlier
record of this stick: additions are assumed, removals are left to you"),
then sections, each with a section-wide checkbox and one per row, each
row with its reason:

1. Playlists to create (parents first), rename, delete
2. Tracks to add to Engine, with cues, rating, comment and cover riding
   along
3. Tracks to remove from Engine (how many playlists hold each)
4. Playlist membership, added and removed, in rekordbox's order
5. Ratings and comments to Engine
6. Cues to Engine
7. Cues and ratings back onto rekordbox (the restores), their own
   section, so the user sees what the export had dropped
8. Conflicts, unchecked, with two buttons in the style of Sync's "use
   these cues"
9. Engine's own, kept: shown, never written

Save label "Sync Engine". After the save the badge is gone, because the
save loop levels the counter and the baseline is recorded.

The badge: `DetectedStickListModel` gains an `engineUpdate` role with
the values `""`, `"cues"` and `"library"`, computed by an `AsyncRequest`
per mounted stick with both catalogs in `MediaController` (started in
`detect()` and `refreshSyncNeeded()`, the latter already called after
every save), never in `data()`, which today runs a file read per call
for `SyncNeededRole` (`media_controller.cpp:128`). With a baseline the
work is an in-memory diff of the Tracks stage the backup advisor already
reads on insert against the baseline's paths, playlists and ratings.
Without one, R against E at the Tracks stage, and the old counter test
for cues. No new disk read on insert. `StickHeaderRow.qml`'s badge reads
"Engine needs syncing" for `"library"` and leads here, "Sync Needed" for
`"cues"` and leads to Sync Cue Points as today.

**Decision (step 12, 2026-10-09):** the summary is
`application::summarizeEngineUpdate`. A baseline whose sequence equals
the pdb's is answered `""` from the two headers alone, with no catalog
read; only a stale one is read in full. Items declined at rekordbox's
present state do not raise the badge, as the page leaves them out.
Playlists come from the Tracks stage memberships, so an empty playlist
rekordbox made, renamed or deleted, and a comment, do not raise it; the
page shows them. The fallback compares by pathKey and playlist path
only: a file the stick lacks still reads `"library"` (the page then
shows it as a refusal).

## Domain

New `src/domain/rekordbox_baseline.{hpp,cpp}` (types, `baselineFrom`,
no I/O) and `src/domain/engine_update_planning.{hpp,cpp}`, both in
`seabass_core` and `seabass_core_testing`.

Types, in outline:

```cpp
enum class ValueOrigin { Rekordbox, Seabass, Unknown };
struct BaselineCue { CuePoint cue; ValueOrigin origin; };
struct BaselineTrack { std::string pathKey, stickRelativePath, analysisFile; std::uint32_t pdbId;
                       std::optional<int> rating; ValueOrigin ratingOrigin; std::string comment;
                       double bpm; std::int64_t durationMs; std::vector<BaselineCue> cues; };
struct BaselinePlaylist { std::uint32_t id, parentId; bool folder; std::string path;
                          std::vector<std::string> members; };   // ordered pathKeys
struct RekordboxBaseline { std::uint64_t pdbSequence; std::string engineUuid; std::int64_t recordedAtUnix;
                           std::vector<BaselineTrack> tracks; std::vector<BaselinePlaylist> playlists;
                           std::map<std::string, std::string> declined; };

struct EngineUpdateItemHeader { std::string key; bool checkedByDefault; bool conflict;
                                EngineUpdateReason reason; std::string reasonText;
                                std::vector<std::string> dependsOn; };
struct TrackToAdd, TrackToRemove, PlaylistCreate, PlaylistRename, PlaylistDelete,
       MembershipEdit (adds with an "after" anchor, removes), MetadataEdit (toward Engine or rekordbox),
       CueEdit (a SyncPlan, either direction), EngineUpdateConflict (the two candidate payloads);
struct EngineUpdateProposal { ...one vector per section...; std::vector<Track> engineOwnKept;
                              bool empty() const; bool onlyCues() const; };
class EngineUpdatePlanner { static EngineUpdateProposal plan(const EngineUpdateInput &); };
```

What it reuses, and what it must not:

- Pairing: `matchTracks(rekordbox, engine, MatchScope::OneStick)`
  (`track_matching.hpp`), then keyed by `normalizedPathKey` of the
  stick-relative path. Engine rows with `streamingSource` are dropped,
  the Sync convention. Two Engine rows for one file: conflict, "Clean Up
  first". A rekordbox file missing on disk: no add, said so; never
  transliterate a path (`docs/write-path-rules.md`).
- Playlists: diff by pdb id between B and R for create, delete and
  rename. Without a baseline a rename is guessed only when parent and
  members agree closely, and the guess is a conflict. An Engine path
  that resolves to more than one playlist
  (`enginePlaylistCountAtPath() != 1`, `engine_playlists.hpp`) is a
  conflict, the rule Browse already enforces. **Decision (review of
  step 13, 2026-10-09):** rekordbox playlists sharing one path are one
  group, since Engine holds one per path (UNIQUE(title, parentListId)).
  The member the baseline knew at that path is paired as usual; the
  rest, all of it without a baseline, and any rename onto a sibling's
  path, is one conflict keyed by its lowest pdb id: create (or fill)
  one Engine playlist with the union, first list first, or leave the
  path alone. Folders, holding no members, merge into one. The pdb
  reader now gives each membership its playlist id, so the lists stay
  apart (`engine_update_planning.hpp`).
- Membership: a three-way merge of the ordered member lists.
  `alignTo()` in `playlist_sync.hpp` is not enough: it counts only
  tracks every library lists and ignores order, and new tracks must be
  members too. Each add carries the preceding member in R that E holds
  or that is being added, so order survives. `sameSong()` handles the
  copy-swap case.
- Metadata: rating and comment in v1. Title, artist and album are v2
  (libdjinterop `set_title`). Key stays out: `set_key` rewrites
  `trackData`, which would create an analysis blob (case j3 in
  `docs/engine-import-prompt.md`).
- Cues to Engine: R's delta against B (added, moved outside tolerance,
  removed), applied onto E's set with `cueToleranceFor(R.bpm, E.bpm)`
  (`cue_tolerance.hpp`), translated with `translateCuesForEngine` and
  compared through `cuesFromEngine` (`engine_cue_translation.hpp`),
  emitted as a `SyncPlan` with `keepExistingColours` so `SyncPlanChange`
  writes it unchanged. A pad whose E value equals neither B nor R is a
  conflict, phrased by `describePadDifference`. Junk cues excluded via
  `isJunkCue` and `MatchingPolicy::ignoreCuesAtStart()`. The pad-at-a-
  memory-cue case is `cuesFromEngine`'s uncertain list within half a
  beat (`SyncPlan::Reason::EngineMemoryOrHotCue`); there is no 500 ms
  rule in the code and none is invented.
- Cues back to rekordbox: B holds cue c with Seabass origin, R lacks it,
  E holds it (via `cuesFromEngine`): a `SyncPlan` toward rekordbox with
  R's cues plus c, reason "rekordbox's export dropped 2 cues Seabass had
  synced from Engine; they go back". Rekordbox origin: the DJ deleted it
  in rekordbox, so remove it from Engine, checked. Unknown: conflict.
  Ratings the same way.
- Reasons are short, positions via `formatCuePosition`, catalog names
  via `catalogDisplayName`, no dashes.

## Writers

All through the staged-edit machinery (`src/gui/edit`, `SaveContext`,
`PendingChange`, `runSaveLoop`, `docs/write-path-rules.md`).

**One owner for the page.** `LibraryEditSession` refuses a change whose
`owner()` differs from the session's editor
(`library_edit_session.cpp:288`). `SyncPlanChange` is owned by "sync",
`MarkRekordboxImportedChange` by "library-health". Add
`src/gui/edit/changes/owned_change.{hpp,cpp}`: wraps any `PendingChange`,
returns the wrapping owner, prefixes `id()`, forwards everything else.
Nothing `dynamic_cast`s changes today. The page's owner is
"rekordbox-export-sync". Side finding to test on its own:
`SyncController::markRekordboxImported()` stages a "library-health"
change into a session that may already hold "sync" changes, which this
rule should refuse; moot in practice since the save loop levels the
counter anyway.

**A new Engine row in an existing library.** `EngineLibraryCreator::create`
(`libdjinterop_engine_library_creator.cpp`) is whole-library only and
two of its helpers are unsafe on a library a player owns:
`markTracksForDeviceAnalysis` runs `UPDATE Track SET isAnalyzed = 0;`
with no WHERE (line 538), and `copyArtworkInto` gives up with
`remove_all(Artwork/)` (line 359). `applyCuesToSnapshot` assumes
44.1 kHz (`DefaultSampleRate`, line 56), 9 percent wrong for a 48 kHz
file. Reusable: the snapshot field mapping, the relative-path handling
with its Windows and UTF-8 cases, the `ensure()` folder walk,
`verifyInformationRowAtIdOne` as a post-write guard. New
`src/infrastructure/engine/engine_track_rows.{hpp,cpp}`:

```cpp
struct NewEngineTrack { domain::Track source; double sampleRateHz; std::string realEngineLibraryPath; };
struct EngineTrackCover { std::function<void(const std::string &)> beforeWrite; bool written; std::vector<std::string> filesWritten; std::string problem; };
std::string engineRelativePath(const std::string &trackFile, const std::string &realEngineLibraryPath);
std::int64_t createEngineTrack(const std::string &writeRoot, const NewEngineTrack &, EngineTrackCover *, std::string *error);
int markForDeviceAnalysis(const std::string &databaseFile, const std::vector<std::int64_t> &ids, std::string *error);  // per id
int removeEngineTrackRows(const std::string &databaseFile, const std::vector<std::int64_t> &ids, std::string *error);
```

(As delivered in step 6: the real library path rides on NewEngineTrack
because a save writes a scratch copy while the row's path is relative to
the stick; the cover's hook and outcome are a required out-parameter.
The cues are written at the file's rate through
`LibdjinteropEngineCueWriter::writeHotCuesAtSampleRate`. A new row has
no trackData until the player analyses it, so it records no rate, and
`LibdjinteropEngineReader` reads its cues at the 44.1 kHz guess: 9% late
for a 48 kHz file until then.)

Cues are written after creation by the shared
`LibdjinteropEngineCueWriter`, one Engine cue path. The sample rate
comes from `TrackMetadataProbe`, never a default. The cover goes through
`repairArtwork` (`engine_artwork.hpp`), which respects file versus
in-database storage and protects what it writes. `AddEngineTrackChange`
declares `m.db` as its backup target and refuses Engine 1.x. The
optional follow-up is switching the creator onto the extracted helpers.

**Removing an Engine row.** `removeEngineTracks` (`engine_playlists.cpp:260`)
calls libdjinterop's `remove_track`, a bare `DELETE FROM Track` without
foreign keys (`track_table.cpp:700`), so PerformanceData survives, and it
refuses tracks that are in a playlist. **That is a latent bug in
`DeleteTracksChange` today.** The right SQL exists in
`tools/engine_import_probe.cpp` (one transaction over PlaylistEntity,
PreparelistEntity, PerformanceData, ChangeLog where it is a table with a
trackId, then Track; Engine's trigger relinks the chains). Lift it into
`removeEngineTrackRows`, fix `removeEngineTracks` with it as a commit of
its own, with a red test showing the leftover rows. AlbumArt rows stay,
they are shared; audio files stay, they belong to rekordbox. Upstream
first for the libdjinterop side (`remove_track` leaving rows behind).

**Engine playlists.** Present: `addToEnginePlaylist`,
`removeFromEnginePlaylist` (append only), `deleteEnginePlaylist`,
`enginePlaylistExists`, `enginePlaylistCountAtPath`,
`engineTrackIdsForFile` (resolves a new row's id from its file at apply
time, which lets a membership change follow an add in the same save).
Add `createEnginePlaylist` (the `ensure()` walk),
`renameEnginePlaylist` (`set_name`, `set_parent`) and
`insertIntoEnginePlaylist` with an "after" anchor (`add_track_after`);
all exist in libdjinterop's `playlist.hpp`. `DeletePlaylistChange` is
reused through `OwnedChange`. `AlignPlaylistChange` only appends and
removes with no order and no new tracks, so `EngineMembershipChange`
is written on top of these instead.

**Metadata.** Engine: `LibdjinteropEngineCueWriter::writeAnnotation`.
rekordbox: a `domain::MetadataRestoreProposal` staged as
`RestoreMetadataChange` through `OwnedChange`; it already handles the
scratch session and the OneLibrary mirror. `export.pdb` has no room for
comments (1160 of 1161 rows have a zero span), so comments reach
OneLibrary only.

**Cues, both directions.** `SyncPlanChange` through `OwnedChange`. Cues
for new tracks go inside `AddEngineTrackChange`.

**The counter.** Nothing to stage: `runSaveLoop` levels it for every
save with both paths.

**Recording the baseline.** `RecordRekordboxBaselineChange(stickRoot,
next)`, staged last, declares the baseline file as a backup target and
registers a hook on a new `SaveContext::onAfterCommit`, run at the end
of `runSaveLoop` after `settleImportLevel`: re-read the pdb sequence,
drop entries whose change id is not among the applied ids, write with
`writeFileDurablyAtomic`. Storage in
`src/infrastructure/local/rekordbox_baseline_file.{hpp,cpp}` with a
required error out-parameter on the read ("safety-critical answers must
not be optional"), a header-only sequence read for the badge, and the
atomic write. The baseline file joins the upfront backups whenever it
exists, so Undo restores it.

**The origin ledger in other saves.** `virtual std::vector<RekordboxWrite>
rekordboxWrites() const` on `PendingChange`, implemented by
`SyncPlanChange` (target rekordbox or OneLibrary),
`RestoreMetadataChange` (rekordbox) and `AddCueChange` (rekordbox).
`runSaveLoop` gathers them for the applied ids and merges them into the
baseline as Seabass origin, advancing the sequence only when the
baseline was current at save start.

Staging order (the save applies in order): create playlists, add
tracks, renames, memberships, metadata, cues to Engine, playlist
deletes, track removals, restores to rekordbox, record baseline.

## GUI

- `src/gui/engine_update_controller.{hpp,cpp}` deriving from
  `StagedCueEditController` (scan request, session attach, staging, undo
  and status for free). `analyze(rekordboxPath, enginePath)` reads
  rekordbox, Engine and OneLibrary at `Detail::Full` through
  `LibraryCatalogCache` (the pattern in `sync_controller.cpp`), the
  rekordbox playlist ids, the Engine playlist list, a new raw-SQL
  `readEngineRowProvenance()` for `pdbImportKey` (kept out of
  `domain::Track`, like the analysis state), the baseline and the import
  state, then runs the planner. `setIncluded`, `setSectionIncluded`,
  `stageSelected()` (honours `dependsOn`: unticking an add unticks its
  memberships), `resolveConflict(index, rekordboxSide)`.
- `src/gui/engine_update_list_model.{hpp,cpp}`: `QAbstractListModel`
  plus `StagedPlanModel`, modelled on `SyncPlanListModel`. Roles:
  section, kind, title, artist, detail, reason, isConflict, included,
  staged, stagedDescription, direction.
- `src/gui/qml/RekordboxExportSyncPage.qml` from `SyncPage.qml`:
  `EditSessionHost { feature: "rekordbox-export-sync"; saveLabel: "Sync
  Engine" }`, `BackBreadcrumb` through `editHost.requestLeave`, a
  `ListView` with `section.property`, `SeabassCheckBox` everywhere, a
  conflict delegate with the reason and two buttons, a link to Sync Cue
  Points when `onlyCues`.
- Routing: `Main.qml` component and handler next to `onSyncRequested`;
  `StickListPage.qml` signal and forwarding; `StickToolCards.qml` card
  in the Sync group; `StickHeaderRow.qml` badge as above.

## Tests

- `tests/engine_update_planning_test.cpp` (plain `assert`, pattern
  `sync_planning_test.cpp`): one case per row of the three-way table and
  per section; a Seabass-origin cue dropped by R gives a restore, a
  rekordbox-origin one gives an Engine removal, unknown gives a
  conflict; a rating restore; declined items persisting; the
  no-baseline fallback; streaming rows ignored; two Engine rows a
  conflict; idempotence (plan, apply in memory, plan again is empty); a
  48 kHz track.
- `tests/rekordbox_baseline_file_test.cpp`: round trip, escaping, sha
  mismatch refused, truncated file refused, header-only sequence read.
- Writer tests on copies of `tests/fixtures/anonymized_library`, never
  the fixture (copy first): `engine_track_rows_test` (a created row
  reads back through `LibdjinteropEngineReader`; `isAnalyzed` 0 and NULL
  blobs on that id only, the count of analysed rows unchanged; the
  Information row still single, not assumed at id 1; removal leaves no
  PerformanceData, PlaylistEntity or PreparelistEntity; the playlist
  chain intact), `engine_playlists_test` (create, rename, insert-after,
  delete), `add_engine_track_change_test` through `runSaveLoop` with
  undo restoring `m.db` and the baseline.
- `tests/engine_update_controller_test.cpp`, pattern
  `playlist_diff_controller_test`: the fixture copy is in "Sync Needed"
  state (pdb 15132, counter 14204) with no baseline; analyze, stage,
  save, re-analyze to empty, baseline present, undo.
- `tests/qml/tst_RekordboxExportSyncPage.qml` with
  `stickFixture.stickCopy`; `tst_StickHeaderRow` for the badge routing;
  `tst_StickToolCards` and `tst_PagesCompile` for the card and page.
- Every test red once without its part of the fix. Lints: no em dashes
  and no spaced double hyphen in string literals, `SeabassCheckBox`
  only, `IconToolButton` for icon-only buttons, `toLocalFileUrl`, no raw
  future reads, UTF-8 path helpers only, SPDX headers, `const` and `let`
  in QML.
- Rig: a check in `tools/rig-edits.sh` that plants a baseline lacking
  one track and removes that track's Engine row (the probe's SQL), drives
  the page, saves, asserts the proposal is empty, undoes, and asserts
  the catalogs are byte-identical to the baseline sha list; via
  `tools/rig_engine_update.cpp` with a `RIG RESULT:` line. Refuses the
  protected sticks.

## Hardware checks owed

1. **Rows Seabass added to a player-written library**, the highest risk:
   the Prime 4 once called a Seabass-created library corrupt and replaced
   `m.db` (2026-09-17). SHAKEDOWN8B: record with
   `engine-import-probe.sh`, sync in the GUI, eject, Prime 4 browses,
   loads and analyses the new track, cover shown, cues in place; record
   and verdict. Then the Go+.
2. **48 kHz cue basis.** On WHALESHARK2 read-only, whether player-imported
   unanalysed 48 kHz rows store quickCues at 44.1 or 48 kHz; then one
   Seabass-created 48 kHz row with a cue at 60 s checked on the deck.
   Decides the sample-rate rule; without it a re-plan after a save may
   propose phantom cue moves.
3. **Removed rows and new or renamed playlists** visible and ordered on
   the deck.
4. **What a rekordbox 7 re-export does to Seabass-written ANLZ cues and
   pdb ratings on unchanged tracks**: the premise of the restore
   section. One planted-cue export on SHAKEDOWN8B from neo, `rig_catalog`
   before and after.
5. **Restores read on a CDJ-3000X and an OMNIS-DUO** (OneLibrary mirror).
6. `seabass-cli sync-after-export --rekordbox P --engine E`: read-only
   proposal with `why:` lines, like `sync --dry-run`; after each
   hardware round the plan must be empty. Evidence under
   `project/evidence/rekordbox-export-sync-<date>/`.

## Order of work

One commit per step, each with the test that goes red without it.
Implementation by Opus 5.5 subagents, one step each, against this plan;
review in the main session.

1. Baseline types and `baselineFrom`, with `rekordbox_baseline_test`. 0.5 d
2. Planner: tracks, playlists, membership, metadata, with the planner
   test cases. 2 d
3. Planner: cues, three-way plus restore plus fallback via
   `SyncPlanner`, with the cue cases. 1.5 d
4. Baseline file I/O and `paths::stickRekordboxBaseline`, with the file
   test. 0.5 d
5. `removeEngineTrackRows`; fix `removeEngineTracks` and
   `DeleteTracksChange` (red test: PerformanceData left behind). 0.5 d
6. `engine_track_rows` create path extracted from the creator, with the
   per-id analysis test. 1.5 d
6b. The Engine reader takes the file's sample rate for rows that have
   no analysis yet. Found in step 6: a row waiting for the player's
   analysis has NULL `trackData`, so nothing in it records a rate, and
   `LibdjinteropEngineReader` assumes 44.1 kHz; the cues of a 48 kHz
   file then read 9 percent late until the player has analysed it. That
   is true of every player-imported unanalysed row on a real stick (the
   anonymized fixture has 1214 of them), not only of rows Seabass adds,
   and it would make the planner propose phantom cue moves right after
   a save. Fix: for a row with cues and no `trackData`, the reader asks
   `TrackMetadataProbe` for the file's rate, cached per stick in the
   existing metadata cache so a 1200-row stick is probed once, with the
   rows that have no cues left alone (nothing to convert). Red test: the
   step 6 case that pins the wrong value today goes green. Also fixes
   today's Sync Cue Points on such rows. 1 d
7. Engine playlist create, rename, insert-after, with tests. 1 d
8. `OwnedChange` and the Add, Remove, Playlist and Membership changes,
   with save-loop tests including undo. 2 d
9. `SaveContext::onAfterCommit`, `RecordRekordboxBaselineChange`, the
   `rekordboxWrites()` ledger, with tests. 1.5 d
10. Controller, list model, controller test. 2 d
11. Page, routing, card, QML tests. 2 d
12. `engineUpdate` role, badge rework, the cheap summary, with
    `tst_StickHeaderRow` and a cost test (no extra catalog read on
    insert). 1 d
13. CLI proposal, `rig_engine_update`, the rig check. 1 d
14. Hardware rounds 1 to 5 and the evidence README. 1 to 2 d with
    Sebastian

About 18 to 20 working days. Experimental until rounds 1 and 2 pass.

## Risks

- New rows in a player's `m.db`. The creator's guards are reused per
  row, never library-wide; see the two unsafe helpers above.
- libdjinterop is a pinned submodule (fork `sebasje/libdjinterop`).
  `remove_track` leaves rows behind; `create_track` hard-codes
  `is_analyzed`. Fixes go upstream first, pins must be reachable.
- `export.pdb` capacity on the reverse direction: `appendPlaylistEntries`
  throws `PdbPageFull` (#62), so v1 restores only cues (ANLZ) and
  ratings (one byte) onto rekordbox, never playlists.
- Code that assumes Engine rows come only from the player's import:
  `cuesFromEngine`'s memory-cue reading is satisfied by rows written
  with `translateCuesForEngine({}, ...)`; `engine_artwork` treats
  `image://fileart` covers as imports; the analysis audit rightly counts
  new rows as unanalysed; the fingerprint and the full-backup advice
  rightly see the stick as changed.
- The counter-based badge goes quiet after any Seabass save; that is why
  the badge becomes baseline-based in step 12.
