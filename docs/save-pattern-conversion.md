<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Converting every save workflow to backup-first, batched writes

## Why

A save should do four things in this order: declare which files it will
overwrite, back all of them up in one durable pass, batch the writes, and
read back what landed. Today no workflow does all four, and the two halves
that exist are implemented in **disjoint** sets of change classes.

| Change class | Batched writes (`FormatWriteSession`) | Backup before first write (`filesToBackup()`) |
|---|---|---|
| `cleanup_group` | yes | no |
| `repair_issue` | yes | no |
| `sync_plan` | yes | no |
| `remove_junk_cue` | no | yes |
| `add_cue` | no | no |
| `copy_cues` | no | no |
| `delete_orphan` | no | no |
| `device_setting` | no | no |
| `merge_cues` | no | no |

Five of nine have neither. Those write to the stick item by item, with the
backup of item *n* landing only after items 1..*n*-1 were already
overwritten -- so a crash or a pulled stick mid-save leaves a half-applied
save with a half-made backup. That is the ordering defect
`filesToBackup()` was added to fix, and it currently fixes it for exactly
one workflow.

The measured stakes, from `docs/write-path-performance.md`: a durable
whole-file write costs about 118 ms on Linux and 15 ms on Windows, and the
backup it leaves behind is 71.5 MB loose against 45.5 MB as one deflated
archive -- permanently, on sticks that were 95% and 97% full in both real
samples. So the conversion is a safety fix that happens to also be the
speed fix.

**Verification is the missing third leg.** Only `OneLibraryCueWriter`
re-reads what it wrote, through a deliberately separate connection.
Nothing else confirms that what reached the stick is what was intended.
That is worth adding once the first two legs are consistent, not before:
a read-back is only meaningful when there is a single commit point to
read after.

## The one structural blocker

Every workflow *can* name its files up front -- checked, not assumed. Five
compute them from data they already hold at construction (`m_path` plus a
track or format), and two need an `export.pdb` lookup that
`remove_junk_cue` already shows how to do from `filesToBackup()` via
`sharedAnlzPathIndex(ctx, path)`.

The blocker is elsewhere. In `copy_cues`, `cleanup_group`, `repair_issue`
and `sync_plan`, the backup paths come from `filesToBackUpFor(sourceId)`,
a lambda living on a context that `makeContext()` builds -- and
`makeContext()` also **constructs the writers**, opens databases and may
take a `FormatWriteSession` scratch copy. Calling it from
`filesToBackup()` would open every writer before the save has decided to
proceed, which is exactly backwards.

**The fix:** split path resolution from writer construction. Give each
format a path-only resolver -- input a format, a root and a track id,
output the files that write would touch -- with no handles, no databases
and no scratch copy. `makeContext()` then uses the same resolver for its
`filesToBackUpFor`, so the two can never drift apart and say different
things about the same write.

That resolver is the whole design. Everything below is applying it.

## Order of conversion

Ordered so each step is provable before the next depends on it.

### Step 0: make the improvement measurable, first

`corpus_test` already has a matrix row per change kind (`matrix.addCue.*`,
`matrix.cleanup.*`, `matrix.repair.*`, `matrix.strayCue.*`,
`matrix.sync.*`). Only `strayCue` records
`durableWritesPerSave`. **Add it to every row before converting
anything**, and confirm each records the pre-conversion number.

This is not ceremony. On the stray-cue path the count did not move when
`filesToBackup()` landed, and that is what exposed a work counter blind to
archive commits -- an apparent 11-to-5 improvement that was really the
metric going dark. Without the guard recorded first, a conversion that
achieves nothing looks identical to one that works.

### The counters cannot see the thing this is mostly for

Measured while converting: `merge_cues` went from 3 durable writes to 2,
but `device_setting` and `delete_orphan` stayed at 1. A change that touches
a single file pays one archive barrier whether the backup is declared up
front or taken on the way past -- the count is identical.

What changes for those two is **ordering**, which no work counter observes.
So the conversion needs a guard of a different kind, and it does not exist
yet:

> Interrupt a save after *n* of *m* items and assert the backup record
> contains all *m* declared files, not the *n* already overwritten.

That is the property the whole conversion is for, and today nothing tests
it for any workflow. `duplicate_cleanup_interruption_test` is the closest
existing shape to copy. Worth writing before converting the harder
workflows, because for the multi-file ones the count improvement and the
ordering fix arrive together and it is easy to accept the visible one as
evidence for both.

### Step 1: the three that need only a declaration

`device_setting`, `delete_orphan` and `merge_cues` name their files from
members plus, for `merge_cues`, the shared ANLZ index. No resolver needed,
no writer touched. `merge_cues` is the same shape as `remove_junk_cue`, so
it is a direct copy of a pattern already in the tree.

Each is a `filesToBackup()` override and nothing else. Expect
`durableWritesPerSave` to drop by roughly the item count.

### Step 2: the path-only resolver

Extract it, and re-point `makeContext()`'s `filesToBackUpFor` at it in the
same commit -- if the two are ever computed separately they will disagree,
and the disagreement is silent: the save backs up one file and overwrites
another.

Cover it directly: for each format, the resolver's answer for a track must
equal the set of files the writer actually opens for that track. That test
is worth more than the conversions it enables.

### Step 3: the four that need the resolver

`copy_cues`, `add_cue`, `cleanup_group`, `repair_issue`, `sync_plan`.
Straightforward once step 2 exists: `filesToBackup()` calls the resolver
for every track the change will touch.

`cleanup_group` needs care -- it can decline to write at all
(`writesToCatalog()`), and a plan that writes nothing must back nothing
up. Declaring files it will not touch is not merely wasteful: it puts
untouched files in the backup record, and undo would then restore files
this save never changed.

### Step 4: batching for the five that lack it

Separate from backup ordering, and worth doing after it. `FormatWriteSession`
already decides for itself whether scratch is worth it
(`shouldUseWholeFileReplace`), so the change per workflow is to route
writes through it rather than to decide anything new.

`add_cue` and `merge_cues` are the ones that matter: both can run over
many tracks. `device_setting` writes one file and should stay direct --
batching a single write buys nothing and adds a commit path to get wrong.

### Step 5: read back what landed

Once every workflow has one commit point, add the third leg. Cheapest
useful form: after commit, re-read each written file through a fresh
reader and compare against what the change intended, the way `corpus_test`
already does with its fresh-reader assertions. `OneLibraryCueWriter`'s
`verifyConnection()` is the existing precedent for why a *separate*
connection matters.

## Rules for the conversion

- **`backupOnce()` stays.** It is the fallback for anything
  `filesToBackup()` missed and it already skips what the upfront pass
  covered. Removing it would turn an incomplete declaration into silent
  data loss instead of a redundant backup.
- **A declaration that is wrong is worse than none.** Too few files means
  an unbacked overwrite; too many means undo restores files the save never
  touched. The resolver test in step 2 is what stops both.
- **Record batch totals, never per-item averages.** An average rounds a
  200-to-1 win to zero. That has cost time four times now.
- **Watch each guard fail.** Break the conversion deliberately and confirm
  the matrix row goes red before trusting that it passed.

## Not in scope

Making `PendingChange::filesToBackup()` pure virtual. Leaving the default
returning nothing means a new change class is merely unoptimised rather
than broken, and the fallback keeps it correct.
