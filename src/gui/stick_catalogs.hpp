#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/find_unreferenced_files.hpp"

namespace seabass::gui
{

struct StickCatalogRead
{
    application::CatalogTracks catalogs;

    // Catalogs whose database is present on the stick but could not be
    // read. Never silently folded into "absent": absent means "this
    // stick has no such catalog, so it protects no files", while a
    // failed read means "this stick HAS one and we do not know what it
    // protects". A caller about to delete files must refuse on a
    // non-empty list rather than proceed with partial knowledge.
    std::vector<std::string> failed;
};

// Reads every catalog present on the stick that `libraryPath` belongs
// to -- rekordbox, Engine and OneLibrary alike -- regardless of which
// one the caller happens to be working in.
//
// `libraryPath` is a catalog directory (".../PIONEER", ".../Engine
// Library"); the stick root is its parent. Reads go through
// LibraryCatalogCache, so a catalog already scanned this session costs
// nothing.
//
// This exists because "is this file still needed?" is a question about
// the stick, never about one catalog.
//
// Note that the three are not three separate libraries: OneLibrary
// (exportLibrary.db, "Device Library Plus") is the same rekordbox
// library as export.pdb in a newer format, so their track lists overlap
// heavily rather than adding up. That makes the single-catalog check
// worse, not better -- the two rekordbox formats do not agree on
// contents. Measured on RV2, where 1468 files on disk are referenced by
// something:
//
//   consulting only rekordbox   protects 1161 -> 307 referenced files
//                                                would look deletable
//   consulting only OneLibrary  protects 1451 ->  17
//   consulting only Engine      protects 1468 ->   0
//
// Those 307 are files the DJ can still play, that a cleanup run from the
// rekordbox side would have permanently destroyed. Engine happens to be
// a superset on this one stick; nothing guarantees that on the next.
StickCatalogRead readAllStickCatalogs(const std::string &libraryPath, application::ProgressReporter &progress,
                                        const application::CancellationToken &cancel);

// When this stick's catalogs were last written, in seconds since the
// epoch, newest wins; 0 when none of them can be read.
//
// The one date a stick can offer about the DJ's own work. Cues, ratings
// and comments live in the catalog databases, so editing any of them
// rewrites the file this reads -- which makes its mtime a real answer to
// "has this stick been worked on since?", the question the last step of
// domain::metadata_merge's rule asks. Per stick rather than per track,
// because nothing finer exists to read.
//
// Newest of the catalogs present, not oldest: a DJ who cues in Engine
// leaves export.pdb untouched for months, and taking the older date
// would date their work to the last time some other program wrote.
//
// stick_backup::libraryCatalogModifiedAt() does the work, plus Device
// Library Plus, which that one has no reason to consult and this one
// does. Not a second list of catalog files: the write-ahead log is the
// whole difficulty here, and one place should know about it.
std::int64_t catalogsLastModified(const std::string &libraryPath);


}  // namespace seabass::gui
