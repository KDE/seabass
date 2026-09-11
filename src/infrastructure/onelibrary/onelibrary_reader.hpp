// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/library_reader.hpp"

namespace seabass::infrastructure::onelibrary
{

// Reads tracks (with cues and playlists) out of Rekordbox's "OneLibrary" /
// "Device Library Plus" format (exportLibrary.db), the third catalog a
// stick can carry alongside the classic Device Library (export.pdb) and
// Engine (m.db) -- see docs/onelibrary-format.md for the schema this is
// based on.
//
// Read-only. Tracks it returns carry format "onelibrary", and the
// write-oriented controllers now understand it: Sync runs the same
// diff+direction logic against it as against rekordbox/Engine, and Clean
// Up, Local Cue Backup, Add Cue and duplicate matching all have a
// OneLibrary path (via OneLibraryCueWriterAdapter, gated on
// OneLibraryCueWriter::existsFor()).
//
// This comment used to say the opposite -- that those four had no path
// for a third format and callers must keep these tracks out of those
// flows. That stopped being true as each one gained a path, and a stale
// capability note is worse than none: it argues for excluding data that
// is now handled correctly.
//
// What is still missing is listed in docs/onelibrary-format.md; the
// short version is that nothing can *create* a row here (see the
// export.pdb row-insertion issue for the same gap on the other side),
// hot loops are refused because the cue table has never been confirmed
// to round-trip them, and colorTableIndex has no known mapping.
class OneLibraryReader : public application::LibraryReader
{
public:
    // pioneerRoot: the stick's "PIONEER" folder, same argument
    // OneLibraryCueWriter takes. Throws if exportLibrary.db doesn't exist
    // for this stick -- callers should check OneLibraryCueWriter::
    // existsFor() first, same convention as the writer.
    explicit OneLibraryReader(std::string pioneerRoot);

    std::vector<domain::Track> readAll() override;

private:
    std::string m_pioneerRoot;
};

}  // namespace seabass::infrastructure::onelibrary
