// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/library_catalog_cache.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include "application/path_key.hpp"
#include "application/ports/library_reader.hpp"
#include "application/use_cases/fill_file_sizes.hpp"
#include "infrastructure/audio/duration_fill.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/file_clock.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;

// The catalog file whose mtime stands in for "has this catalog changed
// since it was last scanned" -- same files SyncController::runAnalyzeTask
// already checks for its own (narrower) staleness purposes.
fs::path freshnessFile(const std::string &format, const std::string &path)
{
    if (format == "rekordbox") {
        return pathFromUtf8(path) / "rekordbox" / "export.pdb";
    }
    if (format == "engine") {
        return pathFromUtf8(path) / "Database2" / "m.db";
    }
    if (format == "onelibrary") {
        return pathFromUtf8(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(path));
    }
    throw std::invalid_argument("LibraryCatalogCache: unknown format \"" + format + "\"");
}

std::chrono::system_clock::time_point realMtime(const std::string &format, const std::string &path)
{
    return infrastructure::toSystemClock(fs::last_write_time(freshnessFile(format, path)));
}

std::unique_ptr<application::LibraryReader> makeReader(const std::string &format, const std::string &path)
{
    if (format == "rekordbox") {
        return std::make_unique<infrastructure::rekordbox::KaitaiRekordboxReader>(path);
    }
    if (format == "engine") {
        return std::make_unique<infrastructure::engine::LibdjinteropEngineReader>(path);
    }
    if (format == "onelibrary") {
        return std::make_unique<infrastructure::onelibrary::OneLibraryReader>(path);
    }
    throw std::invalid_argument("LibraryCatalogCache: unknown format \"" + format + "\"");
}

// What the readers used to do inside every read and do not any more: a
// stat per audio file for its size, and for Engine and OneLibrary a stat
// per artwork to drop the ones not on disk (rekordbox never checked its
// artwork, and still does not, so a Full read is what it always was for
// every format).
// Checked per file, so a stick pulled mid pass (invalidateEveryCatalogOn()
// cancels the prefetch) stops within one stat rather than after every
// other file on it.
void fillSizesAndVerifyArtwork(const std::string &format, std::vector<domain::Track> &tracks,
                               const application::CancellationToken &cancel)
{
    application::fillFileSizes(tracks, cancel);
    if (format != "rekordbox") {
        application::dropMissingArtwork(tracks, cancel);
    }
}

void realStage(LibraryCatalogCache::Detail stage, const std::string &format, const std::string &path,
               std::vector<domain::Track> &tracks, LibraryCatalogCache::StageNotes &notes,
               application::ProgressReporter &progress, application::CancellationToken cancel)
{
    switch (stage) {
    case LibraryCatalogCache::Detail::Tracks: {
        auto reader = makeReader(format, path);
        reader->setProgressReporter(progress);
        reader->setCancellationToken(cancel);
        tracks = reader->readTracks();
        // Every GUI scan goes through here, so this is the one place the
        // lengths neither catalog recorded get filled in -- duplicate
        // detection needs them to tell a radio edit from an extended mix,
        // and the results are cached on the stick so only the first scan
        // pays for it. Doing it here rather than in each controller is
        // deliberate: the fill was once wired into the CLI alone, and the
        // GUI silently found fewer duplicates as a result.
        //
        // Here, the cached lengths only, taken by path: no stat to check
        // them (on a stick with 1200 tracks the catalog does not time,
        // that stat was 2.5 s cold of a stage meant to take a tenth of
        // that) and no probe for a file the cache does not know, which
        // keeps 0 until the Full stage. The Full stage checks the taken
        // ones and probes the rest. A filled-in length is not part of a
        // track's fingerprint, so a fingerprint reads the same at every
        // stage.
        notes.unverifiedDurationPaths =
            infrastructure::audio::fillTrackDurations(tracks, path, application::DurationFill::CachedByPathOnly,
                                                      cancel)
                .unverifiedPaths;
        return;
    }
    case LibraryCatalogCache::Detail::Cues: {
        auto reader = makeReader(format, path);
        reader->setProgressReporter(progress);
        reader->setCancellationToken(cancel);
        reader->fillCues(tracks);
        return;
    }
    case LibraryCatalogCache::Detail::Full:
        cancel.throwIfCancelled();
        fillSizesAndVerifyArtwork(format, tracks, cancel);
        // The lengths the Tracks stage left out: every file the cache did
        // not know is probed now (and cached for the next insertion).
        // Rows the Tracks stage filled already have a length and are not
        // looked at again here.
        infrastructure::audio::fillTrackDurations(tracks, path, application::DurationFill::Complete, cancel);
        // And the cached lengths it took on trust: a file that changed
        // since it was probed is probed again and its rows get the new
        // length. After the fill above, so a file that no longer gives a
        // length is probed once, here, not twice.
        infrastructure::audio::verifyTrackDurations(tracks, path, notes.unverifiedDurationPaths, cancel);
        notes.unverifiedDurationPaths.clear();
        return;
    }
}

int stageNumber(LibraryCatalogCache::Detail detail)
{
    return static_cast<int>(detail) + 1;
}

LibraryCatalogCache::Detail detailOf(int stageNumber)
{
    return static_cast<LibraryCatalogCache::Detail>(stageNumber - 1);
}

}  // namespace

LibraryCatalogCache &LibraryCatalogCache::instance()
{
    static LibraryCatalogCache cache;
    return cache;
}

LibraryCatalogCache::LibraryCatalogCache() : m_stageFn(realStage), m_mtimeFn(realMtime) {}

LibraryCatalogCache::LibraryCatalogCache(StageFn stageFn, MtimeFn mtimeFn)
    : m_stageFn(std::move(stageFn)), m_mtimeFn(std::move(mtimeFn))
{
}

LibraryCatalogCache::LibraryCatalogCache(ScanFn scanFn, MtimeFn mtimeFn)
    : m_stageFn([scan = std::move(scanFn)](Detail stage, const std::string &format, const std::string &path,
                                           std::vector<domain::Track> &tracks, StageNotes &,
                                           application::ProgressReporter &progress,
                                           application::CancellationToken cancel) {
          if (stage == Detail::Tracks) {
              tracks = scan(format, path, progress, std::move(cancel));
          }
      }),
      m_mtimeFn(std::move(mtimeFn))
{
}

LibraryCatalogCache::~LibraryCatalogCache()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        m_prefetchQueue.clear();
        m_prefetchCancel.cancel();
    }
    m_prefetchCv.notify_all();
    if (m_prefetchThread.joinable()) {
        m_prefetchThread.join();
    }
}

std::string LibraryCatalogCache::keyFor(const std::string &format, const std::string &path)
{
    // One key for every spelling of the catalog path: a page hands the
    // native form on Windows, a session's derived sibling the slash
    // form, and invalidateEveryCatalogOn() builds its own. Two keys for
    // one catalog left the stale one behind after a save.
    return format + "\n" + application::normalizedPathKey(path);
}

std::vector<domain::Track> LibraryCatalogCache::tracksFor(const std::string &format, const std::string &path,
                                                            application::ProgressReporter &progress,
                                                            application::CancellationToken cancel)
{
    return tracksFor(format, path, Detail::Full, progress, std::move(cancel));
}

std::vector<domain::Track> LibraryCatalogCache::tracksFor(const std::string &format, const std::string &path,
                                                            Detail detail, application::ProgressReporter &progress,
                                                            application::CancellationToken cancel)
{
    return stagedTracksFor(format, path, detail, progress, std::move(cancel)).tracks;
}

LibraryCatalogCache::StagedTracks LibraryCatalogCache::stagedTracksFor(const std::string &format,
                                                                       const std::string &path, Detail detail,
                                                                       application::ProgressReporter &progress,
                                                                       application::CancellationToken cancel)
{
    const int wanted = stageNumber(detail);
    const std::string key = keyFor(format, path);
    const auto currentMtime = m_mtimeFn(format, path);

    std::unique_lock<std::mutex> lock(m_mutex);

    // A stage this entry already has is served at once, whatever pass is
    // running for it: the advisor's Tracks request must not sit behind
    // the prefetch's eight-second cue pass. Anything else waits while a
    // pass for this key is in flight, whether it is the stage this caller
    // wants or an earlier one (passes run in order, so a later stage can
    // only start once that one lands), rather than starting a second read
    // of the same files.
    for (;;) {
        Entry &entry = m_entries[key];
        const bool fresh = entry.stage > 0 && entry.mtime == currentMtime;
        if (fresh && entry.stage >= wanted) {
            // The stage with the tracks, under this one lock.
            return {entry.tracks, detailOf(entry.stage)};
        }
        if (entry.passInFlight == 0) {
            if (!fresh) {
                // Nothing yet, or read from a catalog file that has changed
                // since: start over from the catalog.
                entry.tracks.clear();
                entry.notes = {};
                entry.stage = 0;
                entry.mtime = currentMtime;
            }
            break;
        }
        // Woken by the pass finishing, an invalidation, or the caller's
        // own token: a page left mid-wait must not keep a worker thread
        // parked behind a cue pass that takes 8 s on a cold stick.
        ++m_waiting;
        m_cv.wait_for(lock, std::chrono::milliseconds(100));
        --m_waiting;
        if (cancel.cancelled()) {
            throw application::OperationCancelled();
        }
    }

    // The missing passes, in order, on this thread. Each works on a copy
    // and commits only once it is complete, so a reader unwinding halfway
    // leaves the previous stage in place and never a truncated one.
    //
    // The generation is captured before releasing the lock: if
    // invalidate() runs on another thread while a pass is in flight (a
    // write's own invalidate() landing while a different controller's
    // already-started read of the same catalog is still running), it bumps
    // m_generation[key] and erases the entry. A pass only commits if
    // nothing bumped it in the meantime; otherwise its result (which may
    // have read data from before whatever just invalidated it) would
    // silently overwrite the invalidation with stale data. A superseded
    // caller still finishes its own read and gets a real, if possibly
    // momentarily stale, answer; only the cache skips it.
    std::vector<domain::Track> work = m_entries[key].tracks;
    StageNotes notes = m_entries[key].notes;
    int have = m_entries[key].stage;
    const std::uint64_t generationAtStart = m_generation[key];
    bool committing = true;
    while (have < wanted) {
        const int next = have + 1;
        if (committing) {
            m_entries[key].passInFlight = next;
        }
        lock.unlock();

        std::exception_ptr error;
        try {
            cancel.throwIfCancelled();
            m_stageFn(detailOf(next), format, path, work, notes, progress, cancel);
        } catch (...) {
            error = std::current_exception();
        }

        lock.lock();
        if (committing) {
            if (m_generation[key] == generationAtStart) {
                Entry &entry = m_entries[key];
                entry.passInFlight = 0;
                if (!error) {
                    // Committed and the next pass claimed (at the top of
                    // the loop) under one lock, so a caller waiting for
                    // a later stage cannot slip in and read it twice.
                    entry.tracks = work;
                    entry.notes = notes;
                    entry.stage = next;
                    entry.mtime = currentMtime;
                }
            } else {
                // Invalidated mid pass: the entry this pass claimed is
                // gone, and one that stands in its place now belongs to
                // whoever created it. Leave it alone and finish uncached.
                committing = false;
            }
            m_cv.notify_all();
        }
        if (error) {
            lock.unlock();
            std::rethrow_exception(error);
        }
        have = next;
    }
    return {std::move(work), detailOf(have)};
}

void LibraryCatalogCache::prefetch(const std::string &format, const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
        return;
    }
    const std::string key = keyFor(format, path);
    const auto sameCatalog = [&](const PrefetchJob &job) { return keyFor(job.format, job.path) == key; };
    if ((m_prefetchCurrent && sameCatalog(*m_prefetchCurrent))
        || std::any_of(m_prefetchQueue.begin(), m_prefetchQueue.end(), sameCatalog)) {
        return;
    }
    m_prefetchQueue.push_back({format, path});
    if (!m_prefetchThread.joinable()) {
        m_prefetchThread = std::thread([this] { prefetchLoop(); });
    }
    m_prefetchCv.notify_all();
}

void LibraryCatalogCache::prefetchLoop()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_prefetchCv.wait(lock, [this] { return m_stopping || !m_prefetchQueue.empty(); });
        if (m_stopping) {
            return;
        }
        m_prefetchCurrent = std::move(m_prefetchQueue.front());
        m_prefetchQueue.pop_front();
        m_prefetchCancel = application::CancellationToken();
        const PrefetchJob job = *m_prefetchCurrent;
        const application::CancellationToken cancel = m_prefetchCancel;
        lock.unlock();

        try {
            tracksFor(job.format, job.path, Detail::Full, application::NullProgressReporter::instance(), cancel);
        } catch (...) {
            // A pulled stick, a cancel, a catalog that does not parse:
            // nothing is cached for the stage that failed, and whoever
            // asks for it next reads it in the foreground and sees the
            // error for itself.
        }

        lock.lock();
        m_prefetchCurrent.reset();
        m_prefetchCv.notify_all();
    }
}

void LibraryCatalogCache::waitUntilPrefetchIdle()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_prefetchCv.wait(lock, [this] { return m_prefetchQueue.empty() && !m_prefetchCurrent; });
}

int LibraryCatalogCache::waitingCallers()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_waiting;
}

std::uint64_t LibraryCatalogCache::invalidationCount(const std::string &format, const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_generation.find(keyFor(format, path));
    return it == m_generation.end() ? 0 : it->second;
}

void LibraryCatalogCache::invalidateLocked(const std::string &key)
{
    // Bumped (never reset) so an in-flight pass of this same key started
    // before this call can tell, once it finishes, that it's no longer
    // safe to cache its result -- see tracksFor()'s own comment.
    ++m_generation[key];
    m_entries.erase(key);
    // A caller waiting on the pass this erased would otherwise wait for a
    // result the cache is no longer going to keep.
    m_cv.notify_all();
}

void LibraryCatalogCache::invalidate(const std::string &format, const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    invalidateLocked(keyFor(format, path));
}

void LibraryCatalogCache::invalidateEveryCatalogOn(const std::string &stickRoot)
{
    if (stickRoot.empty()) {
        return;
    }
    const fs::path root = pathFromUtf8(stickRoot);
    const std::string pioneer = pathToUtf8(root / "PIONEER");
    const std::string engine = pathToUtf8(root / "Engine Library");

    std::lock_guard<std::mutex> lock(m_mutex);
    // The background reads for this stick go first: a queued one would
    // otherwise read the stick straight back in (or fail on a pulled
    // one), and the one in flight stops at its next track.
    const auto onThisStick = [&](const PrefetchJob &job) { return application::pathIsAtOrUnder(job.path, stickRoot); };
    m_prefetchQueue.erase(std::remove_if(m_prefetchQueue.begin(), m_prefetchQueue.end(), onThisStick),
                          m_prefetchQueue.end());
    if (m_prefetchCurrent && onThisStick(*m_prefetchCurrent)) {
        m_prefetchCancel.cancel();
    }
    m_prefetchCv.notify_all();

    invalidateLocked(keyFor("rekordbox", pioneer));
    invalidateLocked(keyFor("onelibrary", pioneer));
    invalidateLocked(keyFor("engine", engine));
}

void LibraryCatalogCache::invalidateWithOneLibraryMirror(const std::string &format, const std::string &path)
{
    invalidate(format, path);
    if (format == "rekordbox") {
        invalidate("onelibrary", path);
    }
}

}  // namespace seabass::gui
