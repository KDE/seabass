<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# What still needs testing by hand

Everything here is unit- or QML-tested and green, and none of it has been
run against a real stick. That distinction matters more than usual in this
project: the bugs that actually hurt (a backup that reports success and
holds nothing, a stick ejected mid-write) live exactly where automated
tests do not reach — real removable media, real crashes, real timing.

Work through a list, then move the item to "Verified" with the date and
what you saw. An item that fails is worth more than one that passes:
write down the message verbatim.

Always start on a **scratch copy** of a stick, never the live one, the
first time through a list.

## Where an individual item lives

This file holds the checklists -- a sequence of steps someone works
through in one sitting. A single change that is blocked on hardware does
not belong here; it belongs on its own GitHub issue, labelled, so it is
findable from the issue tracker rather than from a document nobody opens
until they are already testing.

The labels on `sebasje/seabass`:

| Label | Means |
|---|---|
| `manual-testing` | a human has to exercise this |
| `testing-linux`, `testing-windows` | platform |
| `testing-denon` | Denon / Engine DJ hardware |
| `testing-pioneer-old` | older Pioneer, e.g. XDJ-RX2 -- DeviceLibrary (`export.pdb`) |
| `testing-pioneer-new` | OMNIS-DUO, CDJ-3000 -- OneLibrary (`exportLibrary.db`) |

Pick the hardware label from the **format the change writes**, not from
the machine that happens to be plugged in. Newer players read
`export.pdb` too, but the older parsers are the unforgiving ones, so
`export.pdb` work is `testing-pioneer-old` even when it is first checked
on newer gear.

## 1. Archive concurrency fix — the reason this list exists

Branch `worktree-archive-concurrency`. A Full Stick Backup of a real
26 GB stick produced an archive that reported 12.3 GB and contained
3.8 MB: a listing pass ran crash recovery on the archive mid-backup,
truncating it while the backup kept appending past the new end. The
commit-time verify caught it and the manifest was unreadable, so it was
never offered as restorable — both safety nets held, and the backup was
still lost.

Reproduced in `tests/backup_archive_concurrent_reader_test.cpp` and
fixed (reads never recover; appends refuse to write into a hole). What
the test cannot cover is a real stick at real size with the real UI
running.

- [ ] Leave the Home page open behind you, so the backup advisor keeps
      listing the backup folder in the background. That is what destroyed
      the original. Then run one uninterrupted Full Stick Backup to
      completion. Expect the VERIFIED badge.
- [ ] Check the result is not hollow. `ls -l` the archive and compare
      against `du -h` on the same file: the two should be close. A file
      whose apparent size dwarfs its disk usage is the bug returning.
- [ ] Cancel a backup half way, choose **Keep**, run it again to
      completion, then **Verify**. This is the exact sequence that
      produced the bad archive.
- [ ] Cancel a *first* backup and choose **Discard**: the archive file
      should be gone afterwards.
- [ ] Restore that backup onto a scratch exFAT stick, then take a backup
      preview of the restored stick: it should report zero changes.
      Engine DJ or a player should read the restored stick without
      complaint.

Not fixed, and worth trying to provoke: there is still no lock on an
archive, so a restore, a clone and a backup page writing one archive at
once are only prevented by the app's own sequencing. If you can get two
of those running against the same archive, note what happens.

## 2. Backup USB Stick: create and update from another stick

Merged, never run on hardware. Two scratch sticks, A with a library, B
formatted with an unrelated file on it.

- [ ] B's card offers "Create Backup USB Stick" naming A. Run it. `A.zip`
      appears, B holds the library, B's stray file survives (overlay),
      and a player reads B.
- [ ] Add a cue on A. B now offers "Update Stick" from A. Run it: the
      backup step should be incremental and the copy small.
- [ ] Edit B instead of A, and check A is then offered the update.
- [ ] Edit both, and confirm the diverged warning appears rather than a
      silent overwrite.
- [ ] Watch the two-stage progress, then cancel during the backup stage:
      the partial backup is kept and B is untouched.
- [ ] Try it with a target too small for the library and confirm the card
      is disabled with the space message.

## 3. Auto-mount and eject

- [ ] Insert a stick and confirm Seabass mounts it without being asked.
- [ ] Quit Seabass and confirm it unmounts the sticks it mounted itself,
      and leaves alone one the desktop had already mounted.
- [ ] Eject a stick from the list, re-insert it, and confirm it is
      mounted again.
- [ ] Click eject on one stick while another is busy mounting. It should
      queue, not silently do nothing.

## 4. Windows

- [ ] `WindowsRemovableMediaMounter`'s eject sequence and
      `WindowsRemovableMediaMonitor`'s polling hotplug detection are
      compile- and Wine-verified only. Neither has met a real stick on
      real Windows.
- [ ] Import an XML for a track rekordbox has genuinely never seen. Only
      the already-known-file case has been tested (2026-08-28, worked).

## 5. Free space before any of this

A full backup needs about as much room as the stick holds. Check before
starting a long run rather than after:

```
df -h ~; du -sh ~/Seabass\ Backups
```

## Verified

Nothing yet. First entry goes here with the date and what the badge said.
