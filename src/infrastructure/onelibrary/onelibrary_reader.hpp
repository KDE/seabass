// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
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
// and colorTableIndex has no known mapping.
//
// Cues come from the track's analysis file, not from the database's cue
// table. content.analysisDataFilePath names the same kind of file
// export.pdb's analyze_path does, usually the very same file, and that
// file is what a OneLibrary player shows and writes: an OMNIS-DUO
// (2026-10-01, issue #59) stored pads only in the file and left the cue
// table alone, and a CDJ-3000X (2026-10-03) showed the file where the two
// disagreed. The cue table is read by nothing here; OneLibraryCueWriter
// keeps it in step.
class OneLibraryReader : public application::LibraryReader
{
public:
    // pioneerRoot: the stick's "PIONEER" folder, same argument
    // OneLibraryCueWriter takes. Throws if exportLibrary.db doesn't exist
    // for this stick -- callers should check OneLibraryCueWriter::
    // existsFor() first, same convention as the writer.
    explicit OneLibraryReader(std::string pioneerRoot);

    // readTracks() + fillCues(). No audio file is stat'd for its size
    // (application::fillFileSizes) and no artwork file for its existence;
    // artworkPath is what the catalog names, whether or not the file is
    // still there.
    std::vector<domain::Track> readAll() override;
    // exportLibrary.db alone: every field but the cues and
    // metadataModifiedAt, with each track's analysisFile named.
    std::vector<domain::Track> readTracks() override;
    // The analysis-file pass: each OneLibrary track's cues from the file
    // its analysisFile names (the .EXT and the .DAT, read exactly as the
    // DeviceLibrary reader reads them), and metadataModifiedAt from the
    // .EXT's mtime. A track with no file keeps no cues.
    void fillCues(std::vector<domain::Track> &tracks) override;
    // count(*) of the content table over a read-only open, no journal
    // recovery. Nothing when there is no OneLibrary or it cannot be read.
    std::optional<size_t> countTracks() override;

private:
    std::vector<domain::Track> readCatalog(application::ProgressReporter &progress);
    void readAnalysis(std::vector<domain::Track> &tracks, application::ProgressReporter &progress,
                      const std::string &label);

    std::string m_pioneerRoot;
};

}  // namespace seabass::infrastructure::onelibrary
