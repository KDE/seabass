// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/library_catalog_cache.hpp"

#include <filesystem>
#include <stdexcept>

#include "application/use_cases/scan_library.hpp"
#include "infrastructure/audio/duration_fill.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
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
        return fs::path(path) / "rekordbox" / "export.pdb";
    }
    if (format == "engine") {
        return fs::path(path) / "Database2" / "m.db";
    }
    if (format == "onelibrary") {
        return infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(path);
    }
    throw std::invalid_argument("LibraryCatalogCache: unknown format \"" + format + "\"");
}

std::chrono::system_clock::time_point realMtime(const std::string &format, const std::string &path)
{
    return std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(freshnessFile(format, path)));
}

std::vector<domain::Track> readCatalog(const std::string &format, const std::string &path,
                                        application::ProgressReporter &progress,
                                        application::CancellationToken cancel)
{
    if (format == "rekordbox") {
        infrastructure::rekordbox::KaitaiRekordboxReader reader(path);
        reader.setProgressReporter(progress);
        reader.setCancellationToken(cancel);
        return application::ScanLibrary(reader).execute();
    }
    if (format == "engine") {
        infrastructure::engine::LibdjinteropEngineReader reader(path);
        reader.setProgressReporter(progress);
        reader.setCancellationToken(cancel);
        return application::ScanLibrary(reader).execute();
    }
    if (format == "onelibrary") {
        infrastructure::onelibrary::OneLibraryReader reader(path);
        reader.setProgressReporter(progress);
        reader.setCancellationToken(cancel);
        return application::ScanLibrary(reader).execute();
    }
    throw std::invalid_argument("LibraryCatalogCache: unknown format \"" + format + "\"");
}

std::vector<domain::Track> realScan(const std::string &format, const std::string &path,
                                     application::ProgressReporter &progress, application::CancellationToken cancel)
{
    auto tracks = readCatalog(format, path, progress, cancel);
    // Every GUI scan goes through here, so this is the one place the
    // lengths neither catalog recorded get filled in -- duplicate
    // detection needs them to tell a radio edit from an extended mix,
    // and the results are cached on the stick so only the first scan
    // pays for it. Doing it here rather than in each controller is
    // deliberate: the fill was once wired into the CLI alone, and the
    // GUI silently found fewer duplicates as a result.
    infrastructure::audio::fillTrackDurations(tracks, path);
    return tracks;
}

}  // namespace

LibraryCatalogCache &LibraryCatalogCache::instance()
{
    static LibraryCatalogCache cache;
    return cache;
}

LibraryCatalogCache::LibraryCatalogCache() : m_scanFn(realScan), m_mtimeFn(realMtime) {}

LibraryCatalogCache::LibraryCatalogCache(ScanFn scanFn, MtimeFn mtimeFn)
    : m_scanFn(std::move(scanFn)), m_mtimeFn(std::move(mtimeFn))
{
}

std::string LibraryCatalogCache::keyFor(const std::string &format, const std::string &path)
{
    return format + "\n" + path;
}

std::vector<domain::Track> LibraryCatalogCache::tracksFor(const std::string &format, const std::string &path,
                                                            application::ProgressReporter &progress,
                                                            application::CancellationToken cancel)
{
    const std::string key = keyFor(format, path);
    const auto currentMtime = m_mtimeFn(format, path);

    std::unique_lock<std::mutex> lock(m_mutex);

    // If another thread is already scanning this exact key, wait for it
    // to finish rather than also triggering a scan -- the two most likely
    // near-simultaneous callers (e.g. Stick Statistics kicking off three
    // scans right as Sync Cue Points also opens) would otherwise both pay
    // the real disk-read cost.
    m_cv.wait(lock, [&] {
        auto it = m_entries.find(key);
        return it == m_entries.end() || !it->second.inProgress;
    });

    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.valid && it->second.mtime == currentMtime) {
        return it->second.tracks;
    }

    m_entries[key].inProgress = true;
    // Captured before releasing the lock: if invalidate() runs on another
    // thread while this scan is in flight (e.g. a write's own
    // invalidate() landing while a different controller's already-started
    // scan of the same catalog is still running), it bumps m_generation[key].
    // The write-back below only commits if nothing bumped it in the
    // meantime -- otherwise this scan's result (which may have read data
    // from before whatever just invalidated it) would silently overwrite
    // the invalidation with stale data the moment it finishes.
    const std::uint64_t generationAtStart = m_generation[key];
    lock.unlock();

    std::vector<domain::Track> tracks;
    std::exception_ptr error;
    try {
        cancel.throwIfCancelled();
        tracks = m_scanFn(format, path, progress, cancel);
    } catch (...) {
        error = std::current_exception();
    }

    lock.lock();
    Entry &entry = m_entries[key];
    entry.inProgress = false;
    if (!error && m_generation[key] == generationAtStart) {
        entry.tracks = tracks;
        entry.mtime = currentMtime;
        entry.valid = true;
    }
    // On error, or a generation bump while this scan was running, entry
    // is left exactly as it was: neither a failed re-scan nor a
    // superseded one should evict a previously-good cached result (or
    // resurrect an invalidated one) out from under a concurrent waiter
    // that's about to read it. This call's own return value below is
    // still whatever was actually just scanned -- only the *cache* skips
    // it, the caller still gets a real (if possibly momentarily stale)
    // answer rather than an error.
    lock.unlock();
    m_cv.notify_all();

    if (error) {
        std::rethrow_exception(error);
    }
    return tracks;
}

void LibraryCatalogCache::invalidate(const std::string &format, const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key = keyFor(format, path);
    // Bumped (never reset) so an in-flight scan of this same key started
    // before this call can tell, once it finishes, that it's no longer
    // safe to cache its result -- see tracksFor()'s own comment.
    ++m_generation[key];
    m_entries.erase(key);
}

void LibraryCatalogCache::invalidateEveryCatalogOn(const std::string &stickRoot)
{
    if (stickRoot.empty()) {
        return;
    }
    const fs::path root(stickRoot);
    invalidateWithOneLibraryMirror("rekordbox", (root / "PIONEER").string());
    invalidate("engine", (root / "Engine Library").string());
}

void LibraryCatalogCache::invalidateWithOneLibraryMirror(const std::string &format, const std::string &path)
{
    invalidate(format, path);
    if (format == "rekordbox") {
        invalidate("onelibrary", path);
    }
}

}  // namespace seabass::gui
