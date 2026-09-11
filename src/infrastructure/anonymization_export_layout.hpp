// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <string_view>

// What an anonymized export is allowed to contain, in one place, because
// two pieces of code have to agree about it: the anonymizer decides what
// to copy out of a real stick, and the verifier refuses the export if it
// finds anything else. When those two lists were written separately, a
// real stick carrying a file neither of them knew about (playlists3.sync,
// playlists3Plus.sync, RBFLTR.DAT) produced an export that the verifier
// then refused -- correct, but it made the feature unusable rather than
// dropping the file.
//
// The rule is an allowlist rather than a list of things to remove. A
// rekordbox version that adds a new side file must not be able to leak
// it, and must not be able to block an export either: anything not named
// here is dropped by the anonymizer before the verifier ever sees it.
namespace seabass::infrastructure
{

// Files kept inside <export>/rekordbox/rekordbox/. Everything else in
// that directory is removed.
//
// export.pdb        the catalog, scrubbed in place
// exportLibrary.db  the Device Library Plus mirror, scrubbed in place
//
// Deliberately absent, and each for a reason:
// exportExt.pdb     the My Tag vocabulary -- free text, no anonymizer
// *.sync            rekordbox's playlist sync state -- carries playlist
//                   names, no anonymizer
// RBFLTR.DAT        the saved browse filters -- carries My Tag and
//                   genre selections, no anonymizer
// *-shm, *-wal      SQLite side files, unscrubbed by definition
inline bool isKeptRekordboxCatalogFile(std::string_view name)
{
    return name == "export.pdb" || name == "exportLibrary.db";
}

// Files kept inside <export>/engine/Database2/. Everything else in that
// directory is removed.
//
// m.db  the Engine catalog, scrubbed in place
//
// Deliberately absent, and this one was found shipping real data:
//
// hm.db     the play HISTORY -- its own Track table with real titles,
//           artists, albums and full directory paths, plus which set
//           each track was played in. Nothing anonymized it, because
//           the anonymizer only ever opened m.db, and the whole
//           Database2 directory was copied wholesale. It is not needed
//           for cue sync, cleanup or health, so it goes rather than
//           gaining a second anonymizer to keep correct.
// sm.db,    streaming-service catalogs. Empty in every library sampled,
// stm.db    carry no cue or file data, and would need scrubbing too.
// *-journal SQLite rollback journals: transient by nature, hold
//           PRE-write pages, and are exactly where the content the
//           anonymizer just replaced would survive.
inline bool isKeptEngineDatabaseFile(std::string_view name)
{
    return name == "m.db";
}

}  // namespace seabass::infrastructure
