// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
    // How much of a library a caller needs, in the order the cache reads
    // it: Tracks is the catalog file alone (about 0.1 s cold on a stick),
    // Cues adds rekordbox's ANLZ pass (8 s cold on a 1161-track stick),
    // Full adds every audio file's size (3 s). A request for a stage the
    // entry already has returns at once, whatever pass is in flight; a
    // request for a stage being read waits for that pass rather than
    // starting another; a request for a stage nobody is reading runs the
    // missing passes itself, in order, on the caller's thread. The
    // overload without a Detail is Full.
    //
    // Where a track's durationSeconds comes from, by stage: at Tracks, the
    // catalog's own length, or else the length the stick's duration cache
    // holds for that file, taken by path without looking at the file; a
    // file the cache does not know reads 0. Cues adds no lengths. Full
    // probes every file still at 0 (and caches the answer on the stick),
    // and checks each length the Tracks stage took from the cache against
    // its file, probing again the ones that changed. So only Full has
    // every length there is; a page that groups or compares by length
    // (Duplicates, Clean Up) asks for Full. Filled-in lengths are marked
    // Track::durationIsProbed and are not part of the library fingerprint.
    enum class Detail { Tracks, Cues, Full };

    // What a pass leaves for a later pass of the same entry, kept with the
    // entry and committed with its tracks: the Tracks stage takes cached
    // durations without looking at the audio files, and the Full stage,
    // which stats them anyway, checks those.
    struct StageNotes
    {
        // Files whose cached duration the Tracks stage took unverified.
        std::vector<std::string> unverifiedDurationPaths;
    };

    // One pass of a staged read. For Detail::Tracks, `tracks` arrives
    // empty and the pass fills it from the catalog; for Cues and Full it
    // arrives holding the previous stage's result and the pass adds to
    // it in place. `notes` arrives as the previous pass left it. A pass
    // that throws leaves the cache as it was: it works on copies, so a
    // reader unwinding halfway (a pulled stick, a cancel) never leaves a
    // half-filled stage behind. A pass checks `cancel` at least once per
    // file it touches.
    using StageFn = std::function<void(Detail stage, const std::string &format, const std::string &path,
                                       std::vector<domain::Track> &tracks, StageNotes &notes,
                                       application::ProgressReporter &progress,
                                       application::CancellationToken cancel)>;
    // The one-shot shape the cache had before it had stages, kept for the
    // tests that only care about hit, miss and invalidation: the scan is
    // the Tracks pass and the other two passes add nothing.
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
    // Test seams: inject fakes so staging, hit/miss/invalidate and
    // concurrency behavior can be verified without real stick data or
    // real filesystem timestamps.
    LibraryCatalogCache(StageFn stageFn, MtimeFn mtimeFn);
    LibraryCatalogCache(ScanFn scanFn, MtimeFn mtimeFn);
    // Stops the prefetch worker: cancels the pass it is in and drops the
    // queue.
    ~LibraryCatalogCache();

    LibraryCatalogCache(const LibraryCatalogCache &) = delete;
    LibraryCatalogCache &operator=(const LibraryCatalogCache &) = delete;

    // cancel: also checked every 100 ms while this call waits for another
    // thread's pass (it then throws OperationCancelled), and per track by
    // the reader for the passes this call
    // runs itself (a hit returns at once). A cancelled pass throws
    // application::OperationCancelled and caches nothing for that stage:
    // the next call runs it from scratch, never serving a truncated list.
    // Other callers waiting on the same key are woken and run it for
    // themselves.
    std::vector<domain::Track> tracksFor(const std::string &format, const std::string &path,
                                          application::ProgressReporter &progress =
                                              application::NullProgressReporter::instance(),
                                          application::CancellationToken cancel =
                                              application::CancellationToken::none());

    std::vector<domain::Track> tracksFor(const std::string &format, const std::string &path, Detail detail,
                                          application::ProgressReporter &progress =
                                              application::NullProgressReporter::instance(),
                                          application::CancellationToken cancel =
                                              application::CancellationToken::none());

    // The tracks, and the stage the entry they came from had reached,
    // taken together under one lock: a Tracks request served from an
    // entry that already holds its cues says so in `stage`, and a cue pass
    // committing a moment later cannot make the answer claim cues it does
    // not carry. `stage` is never below the stage asked for.
    struct StagedTracks
    {
        std::vector<domain::Track> tracks;
        Detail stage = Detail::Tracks;
    };
    StagedTracks stagedTracksFor(const std::string &format, const std::string &path, Detail detail,
                                 application::ProgressReporter &progress = application::NullProgressReporter::instance(),
                                 application::CancellationToken cancel = application::CancellationToken::none());

    // Reads the rest of this library in the background, up to Full, one
    // catalog at a time on one worker thread for the whole cache (the FAT
    // driver and the USB queue serialise every read anyway): the prefetch
    // for the features that need it. A foreground tracksFor() for a stage
    // the prefetch is reading waits for it; one for a later stage runs
    // after it. Asking again for a catalog already queued adds nothing.
    // Dropped, with the entries, by invalidateEveryCatalogOn(). A pass
    // that fails in the background is forgotten: the next foreground
    // request runs it and sees the error itself.
    void prefetch(const std::string &format, const std::string &path);

    // Blocks until the prefetch queue is empty and the worker is idle.
    // For tests and for anything that must know the background reads are
    // over; never needed for correctness, since tracksFor() waits for a
    // pass in flight by itself.
    void waitUntilPrefetchIdle();

    // How many tracksFor() calls are waiting right now for a pass another
    // thread is running. For tests, to prove a caller waits rather than
    // reads, without sleeping.
    int waitingCallers();

    // Bumped by every invalidation of this catalog. For tests, to see
    // that an event invalidated a catalog without reading one first.
    std::uint64_t invalidationCount(const std::string &format, const std::string &path);

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
    // wholesale rather than editing one catalog (a backup restore, a
    // stick clone, a format) and for a stick that was pulled: a
    // re-inserted stick whose cues changed elsewhere can keep its
    // catalog's mtime, and must not be served from RAM. Also drops every
    // queued prefetch for a catalog on that stick and cancels the one in
    // flight, so nothing reads a stick that is gone or half rewritten.
    // The mtime comparison alone is not enough: a filesystem's timestamp
    // granularity can be coarser than the gap between the write and the
    // next read.
    void invalidateEveryCatalogOn(const std::string &stickRoot);

private:
    struct Entry
    {
        // Holds everything up to and including `stage`.
        std::vector<domain::Track> tracks;
        StageNotes notes;
        std::chrono::system_clock::time_point mtime;
        // 0 = nothing read yet, else 1 + Detail of the last stage read.
        int stage = 0;
        // 0 = no pass running, else 1 + Detail of the pass another
        // thread is running for this key right now. Always stage + 1.
        int passInFlight = 0;
    };

    struct PrefetchJob
    {
        std::string format;
        std::string path;
    };

    static std::string keyFor(const std::string &format, const std::string &path);
    void invalidateLocked(const std::string &key);
    void prefetchLoop();

    StageFn m_stageFn;
    MtimeFn m_mtimeFn;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unordered_map<std::string, Entry> m_entries;
    // Per-key invalidation counter, incremented by invalidate() and never
    // erased (unlike m_entries) -- lets a pass detect an invalidate()
    // that landed while it was still running, so it doesn't write a
    // since-stale result back into the cache. See tracksFor()'s own
    // comment for the exact race this closes.
    std::unordered_map<std::string, std::uint64_t> m_generation;
    int m_waiting = 0;

    // The prefetch worker, started by the first prefetch(). Guarded by
    // m_mutex; m_prefetchCv wakes the worker and waitUntilPrefetchIdle().
    std::condition_variable m_prefetchCv;
    std::deque<PrefetchJob> m_prefetchQueue;
    std::optional<PrefetchJob> m_prefetchCurrent;
    application::CancellationToken m_prefetchCancel;
    bool m_stopping = false;
    std::thread m_prefetchThread;
};

}  // namespace seabass::gui
