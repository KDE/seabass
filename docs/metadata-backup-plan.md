# Metadata Backup: the DJ's own work, kept off the stick

## Why

Everything a DJ adds to a track lives only on the stick: the cues they set
in the booth, the rating they gave it at 3am, the comment reminding them
what it mixes into. The audio is replaceable -- it can be re-imported from
anywhere. The annotations cannot. They exist in exactly one place, on a
device that gets reformatted, re-exported, dropped and lost.

Seabass already has a narrow version of this (`LocalCueStore`, the "Local
Cue Backup" card, currently marked "Needs rework"). It backs up cues and
nothing else, keys its database off an XDG path rather than `~/Seabass`,
and its restore path merges cue-by-cue with no way for a user to say
"just take the backup's word for it". This is that feature, reworked and
widened to the whole of what a DJ authors.

Two cards, because pulling and pushing are two different decisions taken
at different times:

- **Metadata Backup** -- read every catalog on the stick, store what the
  DJ added, locally.
- **Restore Metadata** -- put it back on a stick that has lost it.

## What gets stored, and what does not

The rule: **store what a DJ authored, plus just enough of what the
software wrote to identify the track again.** Not a second copy of the
library.

| Stored | Why |
|---|---|
| cues and loops (position, kind, hot number, colour, comment) | the whole point |
| rating | authored, and lost on re-import |
| comment | authored |
| play count / last played | authored by playing; the only record of it |
| playlist membership | authored, and cheap to store |
| title, artist, filename, duration, bpm, key | identity, and what a browse view has to show |
| cover art | asked for explicitly, and it makes the browse view legible |
| stick-relative path | for display, and to say where a copy was last seen. Nothing is matched on it |

Deliberately **not** stored: waveforms, beat grids, analysis files,
audio. All of it is derived from the audio file, all of it is large, and
none of it is the DJ's work. `docs/anonymized-export.md` draws the same
line for the same reason.

Cover art is content-addressed: the image is copied to
`~/Seabass/metadata/artwork/<sha256>.<ext>` and the row stores the hash.
A library where 1500 tracks share 300 album covers stores 300 files. The
hash is over the file bytes, so re-running a backup never writes an image
twice.

## Where it lives

```
~/Seabass/metadata/metadata.db          the store
~/Seabass/metadata/artwork/<sha>.jpg    cover art, content-addressed
```

`~/Seabass` is `paths::localRoot()`, which honours the user's preference
and `$SEABASS_HOME`, so a test run can never touch the real store. Both
paths come from `paths::localMetadataDir()`; nothing else invents a
location.

## Identity: how a stored track is found again

This is the part that decides whether the feature works, because a
restore onto a freshly written stick has to recognise tracks it has never
seen a row for.

**Artist, title and length -- the same rule `domain::matchTracks()` uses
everywhere else in Seabass.** The key is normalised artist + title; when
either is missing, the normalised filename stands in, exactly as
matchTracks falls back. Length is not part of the key, because two
readings of one file differ by rounding. It is a guard applied to the
candidates the key finds, with the same 2-second tolerance and the same
"only when both readings are real" rule: a zero duration means unreadable,
not a zero-length track, and gating on it would split one track into two
rows the moment one catalog failed to report a length.

Length earns its place on one real case: a radio edit and an extended mix
share an artist and a title, and without it one would silently inherit the
other's cues.

**Not the file path**, which was this document's first answer and was
wrong. A path is the strongest signal when both sides are looking at one
stick -- which is why `matchTracks` tries it first -- and the weakest
thing to key a store on. The whole point of this store is that it outlives
the stick: a re-export renames folders, a rebuilt library moves
`Contents/` around, and the same track bought again lands somewhere else
entirely. Artist and title travel with the recording; the path does not.

The stick-relative path is still stored, for display and for saying where
a track was last seen. Nothing matches on it -- and `readAll()` leaves
`Track::filePath` empty rather than putting the relative path there,
which is the difference between "the path branch does not fire" and "the
path branch cannot fire". `matchTracks` treats an exact path match as
decisive and needs no other evidence, so an accidental one would be a
worse failure than no match at all.

Both keys are stored per row, not just whichever one was available:
`match_key` for artist + title and `fallback_key` for the filename,
either of which finds the row. See "Schema (v2)" for the case that
forced it.

The store is therefore keyed on the recording and not on which stick it
came from. A track is one row no matter how many sticks carry it; the
`library_id` and `stick_label` columns record where it was last seen, for
the browse view and for nothing else.

## The merge rule

Superseded the per-run conflict policy. That policy asked one question
("take the stick's version" or "keep what is stored?") and applied the
answer to every track in the run. It was the wrong question in both
directions: a run covers 1500 tracks, the stick is newer for some of them
and the store for others, and one answer is wrong for half of them.
Worse, the only honest way to choose was to know which copy held more
work -- which is exactly what the program can see and the person cannot.

One rule now, in `domain/metadata_merge.hpp`, applied per field group in
both directions:

1. **Content beats emptiness.** A field only one side has is not a
   conflict, it is a blank, and a blank is always filled. Nothing ever
   replaces a value with nothing. This is a guarantee rather than a
   preference: a catalog that fails to read a comment reports an empty
   one, and without this line a failed read would erase the comment it
   failed to read.
2. **More cues beats fewer.** A cue set is a body of work rather than a
   single value, so trading four cues for one is a loss however recent
   the one is. Only decides when the two sets actually differ
   (`cueSetsEqual`, which ignores order and sub-second drift).
3. **Otherwise the later modification time wins.**

Ties keep what is already there: two sides that disagree with nothing to
separate them are a coin toss, and a coin toss that rewrites data on
every run is worse than one that leaves it alone.

### Where the dates come from

Both sides of every comparison are a **stick's catalog mtime**, and that
is the point.

A stored value is dated with the mtime of the stick it was read from --
`authored_at`, carried forward -- not with the clock at the moment
Seabass read it. Stamping the clock would date everything a backup run
learned to today, so reading an old stick would make its months-old cues
beat a newer stick's purely by being the one plugged in second. Carrying
the source's date forward keeps the question "which stick was written
later?", which is a question about the DJ's work rather than about the
order they happened to plug things in.

`authored_at` is deliberately not `updated_at`. `updated_at` moves every
time a run touches a row, including runs that write nothing;
`authored_at` moves only when a cue, rating, comment or play count
actually changed. Conflating them meant a routine re-run over an
untouched stick dated the store later than work done elsewhere in
between, and the rule then preferred the store to that work -- silently,
and in the direction that loses the newer cues. Cover art does not count
as authored: it is not the DJ's work, and a cover arriving from a stick
must not re-date that row's cues.

A stick's own date is the newest mtime among its catalog databases, via
`stick_backup::libraryCatalogModifiedAt()` plus Device Library Plus.
**Engine's `m.db-wal` is part of that and must stay part of it:** SQLite
in WAL mode writes commits to the sidecar and only folds them back on a
checkpoint, so a track re-cued in Engine last night can leave `m.db`'s
own mtime weeks old. A rule resting on the stale date would refuse to
back the new cues up *and* offer to overwrite them on a restore.

Newest of the catalogs present rather than oldest: a DJ who cues in
Engine leaves `export.pdb` untouched for months, and the older date would
say their work is older than it is.

0 means "unknown" and never wins a comparison, so a source that cannot
date itself can never overwrite work that can.

### What this changed in practice

A second backup run over an unchanged stick used to rewrite every field
it was handed, deleting and re-inserting every cue of every track,
because "overwrite" did not ask whether the values differed. The rule's
first question is whether the two copies disagree at all, so an unchanged
stick is now read and nothing is written
(`tests/metadata_store_test.cpp`, case 2).

## Restore: what it offers, and how it writes

The restore card's headline case, from the request: **tracks on the stick
that have no cue points at all, for which the store has some.** That is
the unambiguous case, it is common after a re-export, and step one of the
merge rule settles it before any date is consulted.

Beyond it, the same page offers rating and comment -- but not everywhere,
and the difference is measured rather than assumed
(`tests/pdb_rating_write_test.cpp`):

| | rating | comment |
|---|---|---|
| Engine | yes, `set_rating` | yes, `set_comment` |
| OneLibrary | yes, plain SQL | yes, plain SQL |
| DeviceLibrary (`export.pdb`) | yes, a 1-byte field at offset 89 | **no** |

`export.pdb` keeps a comment in a `device_sql_string` whose byte span is
fixed at export time, and the only write available is a re-encode into
exactly that span. So a comment can be shortened or replaced with one
that fits, and never lengthened. On the committed fixture **1160 of 1161
tracks have no comment at all**, which is a span of zero bytes -- so
precisely the tracks a restore would want to give a comment back to are
the ones that cannot take one. Growing the row is the page-allocator
problem `docs/library-health-format-divergence.md` sizes up, not a field
overwrite.

One more thing the format cannot say: rekordbox stores "unrated" and
"zero stars" as the same byte, and the reader maps 0 to "no rating". So
writing a 0 clears a rating rather than setting a zero-star one. The
store keeps the two apart; DeviceLibrary cannot.

Writes are **staged, not applied**. Restore builds one `PendingChange`
per track into the library's `LibraryEditSession`, exactly as
`LocalCueController::applyRestore()` already does; the page's Save writes
them through `runSaveLoop`, which backs up first. Nothing here invents a
write path, and nothing here reaches the stick without the user pressing
Save.

## Browse

"A lightweight way to browse it": one page, one list, a search field over
title/artist/filename. Per row: cover art thumbnail, title, artist,
duration, rating, cue count, and the stick label it was last seen on.
Selecting a row shows its cues.

Lightweight means the list is fed by a SQL query with a LIMIT, not by
reading the whole store into memory and filtering in QML. A store that
has seen a few thousand tracks is small; one that has seen ten sticks
over a year is not.

## Schema (v2)

v2 added `fallback_key` and `authored_at`. On the second, see "Where the
dates come from" above. On the first: v1 stored whichever key happened to be
available, which meant a track catalogued without an artist was filed
under its filename, and when the DJ later fixed the tags the next backup
looked it up under the strong key only, found nothing, and stored a
second row -- with the first row still holding the cues. Two keys per
row, either of which finds it, is what makes that one track instead of
two.

A version-1 store is migrated in place rather than moved aside, which is
the one exception to the pre-1.0 no-migrations rule and exists for the
same reason the move-aside does: this store may hold the only copy of
cues a reformatted stick no longer has, so "we changed the schema" must
not cost them. The change is one added column.

```sql
CREATE TABLE tracks (
  id INTEGER PRIMARY KEY,
  match_key TEXT NOT NULL,         -- normalised "artist|title", empty if the track has neither
  fallback_key TEXT NOT NULL,      -- normalised filename; either key finds the row
  relative_path TEXT NOT NULL,     -- where it was last seen, for display only
  filename TEXT NOT NULL,
  title TEXT, artist TEXT,
  duration_seconds REAL, bpm REAL, music_key TEXT,
  rating INTEGER,                  -- NULL means unrated, never 0
  comment TEXT,
  play_count INTEGER, last_played_at TEXT,
  artwork_sha TEXT,                -- -> artwork/<sha>.<ext>
  artwork_extension TEXT,
  library_id TEXT, stick_label TEXT,
  source_format TEXT,
  first_seen TEXT NOT NULL,
  authored_at TEXT NOT NULL,       -- when a cue/rating/comment last changed, from the SOURCE stick
  updated_at TEXT NOT NULL
);
CREATE TABLE cues (
  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  kind TEXT NOT NULL,              -- 'memory' | 'hot'
  hot_number INTEGER NOT NULL,
  position_ms REAL NOT NULL,
  color TEXT, comment TEXT,
  is_loop INTEGER NOT NULL, loop_end_ms REAL NOT NULL
);
CREATE TABLE playlists (
  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  name TEXT NOT NULL, position INTEGER NOT NULL
);
CREATE TABLE schema_version (version INTEGER NOT NULL);
```

`rating` is nullable and never written as 0 for "unrated" --
`domain::Track::rating` is an `optional<int>` for exactly this reason and
flattening it here would throw the distinction away on the way in.

## What the restore page says out loud

A page that silently declines to write a field teaches the DJ that the
field is unreliable. One that names the format and the reason teaches
them something true about their own library. So when a scan finds tracks
whose comment only DeviceLibrary catalogues, the page says so before the
save rather than the log saying so after it:

> N tracks' comments cannot be put back: they are catalogued only in
> DeviceLibrary, which stores a comment in a fixed space decided when the
> stick was exported and cannot make room for a new one. Their cues and
> ratings still go back. Engine and Device Library Plus take comments of
> any length.

## Order of work

1. `MetadataStore` plus its test: schema, upsert under both policies,
   `readAll()` as a `LibraryReader`, artwork intake. Qt-free.
2. `backUpMetadata()` use case: catalogs in, summary out, progress and
   cancellation through the existing ports.
3. `planMetadataRestore()` use case: store + stick tracks in, per-track
   proposals out. Pure, and the piece most worth testing directly.
4. `MetadataBackupController` and the backup card and page.
5. Browse page.
6. `MetadataRestoreController`, the restore card, staged writes.

## What would make this wrong

- Storing waveforms or analysis files. It is a metadata store; the moment
  it holds derived binary data it is a slow, partial stick backup.
- Keying tracks by path. The feature exists so metadata survives the
  stick, and a path is a fact about a stick.
- Writing to the stick outside the edit session. Every other write path
  in Seabass backs up first; this one does not get an exception.
- A per-track conflict prompt, and equally a per-run one. See the merge
  rule above for why the question itself was wrong.
- Treating a missing field as a conflict. Filling in a blank is not
  overwriting.
- Keying the merge rule on when Seabass looked at a row rather than on
  when the DJ changed it. See `authored_at`.
- A second list of catalog files to stat. `m.db-wal` is the whole
  difficulty; one place should know about it.
- Letting a filename fall back onto another track's row. A strong-key hit
  is decisive even when it ends in no match: artist and title naming a
  row that is then rejected on length means "different mix, new row", not
  "go and look by filename".
- Letting an empty reading overwrite a stored value -- of an authored
  field or of an identity field. A catalog that has lost a track's tags
  reports no title and no artist, and writing that through would blank
  the title on a row that has one and strip it of the key it was found
  by.
- Letting the browse page read the whole store to show twenty rows.

## Two writers against one database, and how that was closed

`RestoreMetadataChange` writes a rating into `export.pdb`. Clean Up and
Sync write the same file through a `FormatWriteSession`, which for a
large enough batch redirects them to a local scratch copy and copies the
whole file back when the save finishes. Both features share one
`LibraryEditSession` per library, so both can land in a single save --
and while each feature kept a session of its own, the two wrote
*different files*. Whichever committed last won: the session's copy, made
before the rating was written, silently replaced it while the page
reported the restore as applied.

The same hole was open for Engine and OneLibrary, not just `export.pdb`:
any change writing a catalog database directly loses to another change's
scratch commit.

`FormatWriteSession`'s own class comment had promised the fix all along
-- "every change of one save that writes the same catalog uses the same
copy" -- but each feature obtained its session under a per-feature key,
so the promise held only *within* a feature. It is now obtained through
`sharedFormatWriteSession()`, keyed on the database file. One copy, one
commit, nothing to race, and every writer in this feature points at
`session.writeRoot()` rather than at the stick.

Two smaller things fell out of it. The rating is written and committed
per rated track rather than buffered across the save: `PdbRowWriter`
reads the whole file at construction and refuses to commit if anything
changed underneath, so holding one open across a save that another change
also writes is a guaranteed loser. And rekordbox cues still go to the
real catalog folder, because they live in per-track ANLZ files rather
than in `export.pdb` -- the same split `makeContext()` draws in Clean Up.

`tests/restore_metadata_change_test.cpp` case 7 drives it: a session that
really is scratching, a real restore change, and the rating read back off
the stick's own `export.pdb` afterwards.

### OneLibrary cannot use a scratch copy at all

Found while testing the fix above, and it outranks it. `exportLibrary.db`
is a **WAL** database -- `PRAGMA journal_mode` returns `wal` on the
committed fixture. A save holds its writers open across the commit, so in
WAL mode the rows it wrote are still sitting in `exportLibrary.db-wal`
waiting to be checkpointed, while `FormatWriteSession` commits by copying
the single `.db` file back.

That loses this format's writes in **both** directions, which is why it
took a test rather than an argument to see:

- writes routed through the scratch copy are stranded in a `-wal` file
  nobody copies;
- writes sent to the real file instead are overwritten by the scratch
  copy when it is committed.

Both were measured. The second had a test passing over it for a while
because the synthetic fixture it used was not in WAL mode, unlike a real
stick's database.

So `sharedFormatWriteSession()` now forces `itemCountHint` to 0 for
`onelibrary`, whatever the caller asked for: the session declines the
scratch copy, `writeRoot()` is the real root, and every writer in the
save agrees on one file again. The optimisation was never actually
available for this format -- it only looked available, and looked it
while quietly dropping writes. Engine's `m.db` is a rollback-journal
database and keeps its scratch copy.

This is not specific to Restore Metadata: Clean Up, Sync and Library
Health repair all route OneLibrary writes through the same sessions, so
the same loss was reachable from any of them. The real fix, if the
optimisation is ever wanted back, is for `FormatWriteSession` to
checkpoint and close a format's connections before copying -- or to copy
the `-wal` and `-shm` alongside the database.

### The same WAL premise reaches the backups, and they have not moved

If `exportLibrary.db` is a WAL database, then a copy of that file alone
is not a copy of the database. Every backup path in Seabass still takes
exactly that copy: `SaveContext::backupOnce()`, `backupAllNow()` and
`filesWrittenFor()` all name the `.db` and never `-wal` or `-shm`.

The failure needs an uncheckpointed `-wal` on the stick, which is what
being killed or unplugged with a connection open leaves behind. A later
save then archives a `.db` that is missing the rows still sitting in that
`-wal`. Restoring that archive puts the older `.db` back while the newer
`-wal` is still there, and SQLite replays the one on top of the other --
so the restore either quietly does not restore, or trips the WAL
checksum.

Not introduced by this work, and not fixed by it: it is the same premise
the scratch-copy finding above rests on, followed into the other half of
the write path. Backing up a WAL database means capturing its sidecars
with it, and restoring means putting them back together or checkpointing
before the copy. That belongs with the backup format, not here.

### What is still on the old footing

Sharing the session fixes it for the features that take theirs from
`sharedFormatWriteSession()`. Four changes still write their catalog at
the real root and so still lose to another change's scratch commit in
the same save:

| Change | Writes directly |
|---|---|
| `AddCueChange` | OneLibrary, and Engine's `m.db` |
| `RemoveJunkCueChange` | OneLibrary |
| `CopyCuesChange` | OneLibrary |
| `MergeCuesChange` | its catalogs, and the OneLibrary mirror |

The concrete case, unchanged by this work: add a hot cue to an Engine
track, stage a twenty-group Engine Clean Up, save once. The cue goes to
the stick's `m.db`; the Clean Up's scratch copy, taken before it, is
committed on top; the cue is gone and the page said it was written.

The OneLibrary mirror is the same story from the other side.
`exportLibrary.db` is deliberately not behind a session anywhere,
including here -- every mirror write in the codebase passes the real
PIONEER root to `sharedOneLibraryWriter()`, so they at least all agree
on one file and one writer. Putting only this feature's mirror behind a
session would break that agreement rather than settle it: it would write
a copy the others do not, and whichever committed last would win.

Converting the four, and then the mirror, is the rest of this seam. It
was left out of this change deliberately -- each needs the same care
about which writers take the write root and which stay on the real one
(rekordbox cues live in ANLZ files; only the catalog database moves),
and that is a change to four destructive features, not a detail to slip
in alongside a metadata feature.

One more thing this made ordering-dependent. The scratch decision is
taken once, by whichever change reaches the session first, from that
change's `itemCountHint`. Stage a rekordbox Sync or a single metadata
restore before a five-hundred-item Clean Up and the Clean Up loses its
scratch copy -- five hundred direct `export.pdb` rewrites instead of
one. It costs speed and never correctness, but it is a coin flip rather
than a bound. Taking the largest hint seen, and deferring the decision to
the first write, would remove it.

## Relationship to Local Cue Backup

`LocalCueStore`, `LocalCueController`, `LocalCuePage.qml` and
`domain::LocalRestorePlanner` are the narrow ancestor of this and are
already marked deprecated in the UI. They should go once this ships --
pre-1.0, and two features that back up overlapping data to two different
databases is worse than either alone. That removal is deliberately not
part of this branch: it is a separate decision, and it wants the new path
proven on real sticks first.
