<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Ratings + comments (experimental) -- feature request & verification plan

Branch: `feature/master-db-ratings-comments`, branched from
`feature/anonymize-library-testdata` (not `main` -- `main` is well behind,
still on the old "DjConvertGui" module name; branching from the WIP branch
picks up everything landed there instead of building on a stale base).

## Feature request, as given (2026-09-02)

### Goal
Set star ratings and comments on tracks, both during a set (on the
Prime 4) and during library prep at home.

### Established facts
- Engine DJ 5.0 added on-device star ratings (Library + Performance
  views). Clearing a rating = swipe left past the first star.
- Comments are NOT editable on-device. Read-only there; no ETA.
- Engine DJ Desktop can set both. Clear a rating by clicking left of
  the first star.
- rekordbox can set both, and BOTH cross over in the USB conversion
  (rating and comment are among the fields that survive).
- Engine never writes ratings/comments back to file tags, and never
  writes back to /PIONEER/export.pdb. Deck-side edits live only in
  `Engine Library/Database2/m.db` on the stick.
- Re-import is re-triggered by rekordbox export changes (confirmed
  empirically). So deck-side ratings can be silently wiped.

### Design decision
Source of truth = rekordbox `master.db` (via pyrekordbox). Writes go
there; they propagate downstream to Engine on next export. Never write
to m.db as a primary store.

Deck-side capture is a SEPARATE mode: read m.db after a gig, diff
against rekordbox, merge upward. It's a read + merge, not a write.
This is the part that's genuinely experimental -- it depends on m.db
surviving until it's pulled.

### To verify before coding (snapshot-and-diff, same method as the
### re-import test: set a known value in the GUI, diff the DB)
1. Rating encoding. rekordbox XML uses 0/51/102/153/204/255. Unknown
   whether `djmdContent.Rating` in master.db uses that scale or 0-5.
   Check both sides.
2. Which column in the Engine `Track` table holds the comment.
3. Whether a re-import actually clobbers m.db ratings, or merges.
   (Related to the open question about what re-import does to
   deck-set hot cues.)

### UI caveat to show on the deck-side path
"Ratings set on the Prime 4 are stored only on the USB stick and are
erased the next time rekordbox re-exports to it."

## Scope note (flagged, not yet resolved)

Everything Seabass does today is scoped to *USB stick* contents --
DeviceLibrary (`export.pdb`), OneLibrary (`exportLibrary.db`), Engine
(`m.db`), all read from a mounted stick. `master.db` is a different
thing entirely: rekordbox's own local collection database, living on
the DJ's computer, never on a stick. Reading/writing it is a genuine
scope expansion -- worth being deliberate about, not just backed into
via this one feature. Not resolved yet; the user chose to defer this
discussion and go straight to prepping the verification work instead.

## What's confirmed so far (this session, 2026-09-02)

- No existing code in this repo touches `master.db` at all.
  `pyrekordbox` is referenced only as a cross-check reference in
  comments (`onelibrary_key.cpp`, `rekordbox_settings_fields.hpp`) for
  algorithms already reimplemented natively in C++ (base85 decode,
  checksum) -- never an actual runtime dependency. No Python
  dependency exists anywhere in this project.
- Neither `sqlite3` (the CLI) nor the `pyrekordbox` Python package are
  installed on this Linux dev machine.
- A real `master.db` **is** reachable from this machine, via the
  Windows partition (dual-boot):
  `/media/sebas/Windows/Users/sebas/AppData/Roaming/Pioneer/rekordbox/master.db`
  (also present under `Program Files/Pioneer/rekordbox 6.8.6/` and
  `Program Files/rekordbox/rekordbox 7.2.18/` -- those are almost
  certainly the empty/template DB shipped with the installer, not the
  real collection; the one under `AppData/Roaming` is the real one).
- Confirmed encrypted: the file's first bytes are high-entropy, no
  `SQLite format 3\0` magic string at all -- consistent with the
  well-known fact that `master.db` is SQLCipher-encrypted from byte 0
  (unlike a plain SQLite file, which is only page-encrypted after a
  cleartext header). Real key-derivation work is needed to open it;
  OneLibrary's existing `sqlcipher_dyn.hpp`/`onelibrary_key.cpp`
  crypto *primitives* (dynamic libsqlcipher loading, key handling
  scaffolding) may be reusable, but `master.db`'s own key-derivation
  scheme needs its own verification -- it's a much older, separately
  evolved format from OneLibrary's, not a given that they match.
- Real, already-existing snapshots sit right next to the live file:
  `master.backup.db` (Aug 28 15:40), `master.backup2.db` (Aug 28
  15:31), `master.backup3.db` (Aug 28 14:30) -- rekordbox's own
  autosave/backup mechanism. These could support a passive diff
  (comparing two already-existing snapshots) without needing a live
  edit, but the user specifically asked to hold off on touching/
  decrypting any of this until they can do a **controlled** edit
  instead (see below) -- don't decrypt or diff these without that.

## Next step (blocked on the user, tonight)

Per the user's own "snapshot-and-diff" method: they'll boot into
Windows, open the real rekordbox app, and set one specific **known**
rating + comment on one specific, identifiable track (title/artist they
can name). Take a snapshot of `master.db` immediately before and after
that one edit. Bring both snapshots (or just the path, if this Linux
session can read the Windows partition live at diff time) back here for
diffing -- this directly answers verification questions 1 and 2 above
with an unambiguous before/after signal, rather than hoping the
already-existing backups happen to bracket a real edit.

Reminder scheduled for this evening to follow up.
