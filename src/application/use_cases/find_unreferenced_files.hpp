// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::application
{

// One audio file found on the stick, as the filesystem sees it -- no
// catalog knows about it yet, which is the whole point.
struct AudioFileOnDisk
{
    std::string filePath;             // absolute, as walked
    std::uint64_t fileSizeBytes = 0;  // 0 when it could not be stat'ed
};

// Every catalog on the stick, named one by one rather than handed over as
// a single pre-merged list.
//
// The shape is the safety rule. A file is only unreferenced if *no*
// catalog mentions it, so checking one format and acting on the answer
// would delete files another catalog still plays -- the one mistake in
// this feature that cannot be walked back. A single
// `std::vector<Track> allTracks` parameter makes that mistake invisible
// at the call site; three named fields make a caller that forgot one
// leave a visibly empty field, and findUnreferencedFiles() reports which
// catalogs it actually consulted so the omission reaches the screen
// instead of dying in a log.
//
// nullopt means "this catalog is not on this stick at all"; an empty
// vector means "present, and it has no tracks". Both are ordinary --
// plenty of sticks are rekordbox-only -- but they are not the same
// thing, and only the second is evidence about the files on disk.
struct CatalogTracks
{
    std::optional<std::vector<domain::Track>> rekordbox;
    std::optional<std::vector<domain::Track>> engine;
    std::optional<std::vector<domain::Track>> oneLibrary;

    // Names of the catalogs actually supplied, in a fixed order.
    std::vector<std::string> present() const;
};

struct UnreferencedFileScan
{
    // Files on disk that no supplied catalog references. Empty (and
    // `usable` false) when no catalog was supplied at all.
    std::vector<AudioFileOnDisk> unreferenced;

    // Which catalogs the answer is based on -- for display, so a run
    // that only saw rekordbox can say so rather than presenting a
    // half-checked list as if it were complete.
    std::vector<std::string> catalogsConsulted;

    std::size_t audioFilesSeen = 0;
    std::size_t referencedPathsSeen = 0;

    // False when CatalogTracks held nothing at all. Callers must treat
    // that as "we do not know", never as "nothing is referenced" -- the
    // latter would propose deleting every audio file on the stick.
    bool usable = false;
};

// Pure decision: no filesystem access, no database access. The caller
// walks the disk (see infrastructure/cleanup/audio_file_walk.hpp) and
// reads the catalogs; this only subtracts one from the other.
//
// Path comparison is separator- and case-normalized. Case matters
// because both exFAT and NTFS are case-insensitive: a catalog row saying
// "Contents/A/b.mp3" and a directory entry saying "Contents/a/b.mp3" are
// the same physical file, and a case-sensitive comparison would call
// that file unreferenced and offer it for deletion. Normalizing case can
// only ever move a file from "unreferenced" to "referenced", which is
// the harmless direction.
//
// Streaming rows (Engine/TIDAL) are deliberately NOT filtered out of the
// referenced set even though their filePath points at a cache on some
// other machine rather than at the stick. Skipping them could only ever
// move a file toward "unreferenced", and nothing here is worth spending
// that risk on.
UnreferencedFileScan findUnreferencedFiles(const std::vector<AudioFileOnDisk> &filesOnDisk,
                                             const CatalogTracks &catalogs);

}  // namespace seabass::application
