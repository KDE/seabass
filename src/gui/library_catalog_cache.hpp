// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"

namespace seabass::gui
{

// Caches the result of scanning a catalog (rekordbox/engine/onelibrary),
// keyed on (format, path), so the many controllers that each independently
// call ScanLibrary today (8 controllers, 26 call sites at the time this was
// written) can share one real disk read per catalog per session instead of
// re-scanning the same removable-media data on every page open.
//
// Lives in gui/, not domain/application: constructing the right
// infrastructure reader for a given format string is exactly the job every
// controller's own scan-task function already does (see e.g.
// SyncController::runAnalyzeTask), just consolidated here instead of
// duplicated per controller.
//
// tracksFor() is blocking -- call it from whatever background thread
// (QtConcurrent::run task) used to call ScanLibrary directly, same as
// before. It is safe to call concurrently from multiple threads for
// different (or the same) keys: a second caller for a key another thread
// is already scanning waits for that scan to finish rather than triggering
// a redundant one.
//
// Freshness is checked by comparing the catalog's own database file's
// mtime against what was cached; a write elsewhere (Sync's apply(), Clean
// Up, ...) should call invalidate() explicitly right after, rather than
// relying purely on the next mtime check, so a caller never has to wait
// out a filesystem timestamp granularity window to see its own write.
//
// progress is only ever touched on a cache miss (a hit returns instantly,
// nothing to report) -- passed in per-call rather than held as cache
// state, so the cache itself stays agnostic to which controller's
// progress properties a given caller wants updated. Callers that don't
// care (StickStatisticsController's own scan reports no progress today
// either) can omit it and get NullProgressReporter.
class LibraryCatalogCache
{
public:
    using ScanFn = std::function<std::vector<domain::Track>(const std::string &format, const std::string &path,
                                                              application::ProgressReporter &progress,
                                                              application::CancellationToken cancel)>;
    using MtimeFn =
        std::function<std::chrono::system_clock::time_point(const std::string &format, const std::string &path)>;

    // One shared, process-wide instance -- controllers are QML-instantiated
    // (e.g. `SyncController { id: syncController }`), so there's no single
    // C++ construction point to inject a shared cache through; a singleton
    // avoids threading a pointer through every page's QML property list for
    // what is, from any one controller's point of view, a passive,
    // stateless-looking dependency.
    static LibraryCatalogCache &instance();

    // Real behavior: constructs the matching infrastructure reader
    // ("rekordbox"/"engine"/"onelibrary") and stats that catalog's own
    // database file.
    LibraryCatalogCache();
    // Test seam: inject fakes so cache hit/miss/invalidate/concurrency
    // behavior can be verified without real stick data or real filesystem
    // timestamps.
    LibraryCatalogCache(ScanFn scanFn, MtimeFn mtimeFn);

    // cancel: checked per track by the reader on a cache miss (a hit
    // returns at once). A cancelled scan throws application::
    // OperationCancelled and caches nothing -- the next call scans from
    // scratch, never serving a truncated list. Other callers waiting on
    // the same key are woken and scan for themselves.
    std::vector<domain::Track> tracksFor(const std::string &format, const std::string &path,
                                          application::ProgressReporter &progress =
                                              application::NullProgressReporter::instance(),
                                          application::CancellationToken cancel =
                                              application::CancellationToken::none());

    // Call after writing to this catalog (Sync's apply()/applyOne(), Clean
    // Up writes, ...) so the next tracksFor() re-scans unconditionally
    // instead of trusting a possibly-stale mtime comparison.
    void invalidate(const std::string &format, const std::string &path);

    // Same as invalidate(), plus "onelibrary" at the same path when
    // format == "rekordbox" -- the shape every rekordbox-primary write
    // path that also best-effort mirrors cues into OneLibrary's
    // exportLibrary.db needs (Clean Up, Local Cue restore, Add Cue), so
    // that mirrored cache entry doesn't go stale even though it's never
    // the format actually being edited. A no-op mirror invalidation for
    // any other format.
    void invalidateWithOneLibraryMirror(const std::string &format, const std::string &path);

    // Every catalog on one stick at once, for a write that replaced files
    // wholesale rather than editing one catalog: a backup restore, a
    // stick clone, a format. Those all go through paths that never knew
    // which formats they touched, and the mtime comparison alone is not
    // enough -- a filesystem's timestamp granularity can be coarser than
    // the gap between the write and the next read.
    void invalidateEveryCatalogOn(const std::string &stickRoot);

private:
    struct Entry
    {
        std::vector<domain::Track> tracks;
        std::chrono::system_clock::time_point mtime;
        bool valid = false;
        bool inProgress = false;
    };

    static std::string keyFor(const std::string &format, const std::string &path);

    ScanFn m_scanFn;
    MtimeFn m_mtimeFn;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unordered_map<std::string, Entry> m_entries;
    // Per-key invalidation counter, incremented by invalidate() and never
    // erased (unlike m_entries) -- lets tracksFor() detect an invalidate()
    // that landed while its own scan was still running, so it doesn't
    // write a since-stale result back into the cache. See tracksFor()'s
    // own comment for the exact race this closes.
    std::unordered_map<std::string, std::uint64_t> m_generation;
};

}  // namespace seabass::gui
