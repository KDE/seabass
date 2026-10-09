// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/library_catalog_cache.hpp"

#include "gui/running_reads.hpp"

#include <algorithm>
#include <atomic>
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
#include "infrastructure/local/cached_sample_rates.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"

#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif

namespace seabass::gui
{

namespace
{

// Passes a pass's progress on to its own caller and keeps a copy for the
// callers waiting for that pass (LibraryCatalogCache::PassProgress, #67).
class RecordingReporter : public application::ProgressReporter
{
public:
    RecordingReporter(application::ProgressReporter &inner, LibraryCatalogCache::PassProgress &record)
        : m_inner(inner), m_record(record)
    {
    }
    void start(const std::string &label, size_t total) override
    {
        {
            std::lock_guard<std::mutex> lock(m_record.mutex);
            m_record.label = label;
            m_record.total = total;
            m_record.current = 0;
            ++m_record.stretches;
        }
        m_inner.start(label, total);
    }
    void tick(size_t current) override
    {
        {
            std::lock_guard<std::mutex> lock(m_record.mutex);
            m_record.current = current;
        }
        m_inner.tick(current);
    }
    void finish() override { m_inner.finish(); }
    void phase(const std::string &label) override { m_inner.phase(label); }
    void warn(const std::string &message) override { m_inner.warn(message); }

private:
    application::ProgressReporter &m_inner;
    LibraryCatalogCache::PassProgress &m_record;
};

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

// The analysis files a cue pass over `tracks` reads: each row's .DAT and
// .EXT under the PIONEER root, the ones a player rewrites when the DJ
// stores a pad (XDJ-RX2, OMNIS-DUO, CDJ-3000X; see
// docs/onelibrary-format.md). Null for Engine, whose cues are in m.db.
std::shared_ptr<const std::vector<std::string>> analysisFilesOf(const std::string &format, const std::string &path,
                                                                const std::vector<domain::Track> &tracks)
{
    if (format != "rekordbox" && format != "onelibrary") {
        return nullptr;
    }
    auto files = std::make_shared<std::vector<std::string>>();
    files->reserve(2 * tracks.size());
    const fs::path root = pathFromUtf8(path);
    for (const domain::Track &track : tracks) {
        if (track.analysisFile.empty()) {
            continue;
        }
        for (const bool ext : {false, true}) {
            files->push_back(
                pathToUtf8(root / pathFromUtf8(infrastructure::rekordbox::anlzRelativePath(track.analysisFile, ext))));
        }
    }
    // A DeviceLibrary row and a OneLibrary row can name one file; once is
    // enough.
    std::sort(files->begin(), files->end());
    files->erase(std::unique(files->begin(), files->end()), files->end());
    return files;
}

// The state of those files, as one number that moves when any of them is
// rewritten, replaced, created or removed: each file's mtime and size,
// or that it is missing, folded in order. A stat per file, no directory
// read: a file rewritten in place does not move its directory's mtime
// (checked on a fixture copy: neither a touch nor an in-place write moved
// the track folder's or the P folder's; only a replace by rename moved
// the track folder's), so the directories cannot stand in for the files.
// About 2300 stats on the 1161-track fixture: 10 ms warm, possibly
// seconds cold on a slow stick, hence the token, looked at every hundred
// files. Nothing when it was set before the end.
std::optional<std::uint64_t> analysisStateOf(const std::vector<std::string> &files,
                                             const application::CancellationToken &cancel)
{
    std::uint64_t state = 1469598103934665603ULL;
    const auto fold = [&state](std::uint64_t value) {
        state ^= value + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
    };
    size_t looked = 0;
    for (const std::string &file : files) {
        if (looked++ % 100 == 0 && cancel.cancelled()) {
            return std::nullopt;
        }
        std::error_code ec;
        const auto written = fs::last_write_time(pathFromUtf8(file), ec);
        if (ec) {
            fold(0);
            continue;
        }
        const auto size = fs::file_size(pathFromUtf8(file), ec);
        fold(static_cast<std::uint64_t>(written.time_since_epoch().count()));
        fold(ec ? 0 : static_cast<std::uint64_t>(size));
    }
    return state;
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

// The filesystem identity of the stick an Engine library is on, for the
// reader's local copies of covers kept in the database. Empty for a library
// in Seabass's own tree on this computer (a browsed backup), which has no
// stick of its own, and when the system knows no identity for the stick
// (readStickHardwareInfo()'s fallback, label plus size, which clones share).
std::string volumeIdentityOf(const std::string &engineLibraryPath)
{
    // "…/Engine Library/" names the same folder as "…/Engine Library".
    fs::path library = pathFromUtf8(engineLibraryPath).lexically_normal();
    if (!library.has_filename()) {
        library = library.parent_path();
    }
    // Inside Seabass's own tree, as a whole path component: ~/Seabass2 is
    // not inside ~/Seabass.
    const fs::path inside = library.lexically_relative(infrastructure::paths::localRoot().lexically_normal());
    if (!inside.empty() && *inside.begin() != "..") {
        return {};
    }
    const std::string identity =
        infrastructure::system::readStickHardwareInfo(pathToUtf8(library.parent_path()), std::string()).stickIdentifier;
    return identity.rfind('-', 0) == 0 ? std::string() : identity;
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
        auto *engine = dynamic_cast<infrastructure::engine::LibdjinteropEngineReader *>(reader.get());
        if (engine == nullptr) {
            reader->fillCues(tracks);
            return;
        }
        // Engine's cues came with the Tracks stage, which opens no audio
        // file, so a row the player has not analysed yet (no sample rate
        // recorded) has them at the reader's 44.1 kHz guess. Here its file
        // says: only for the rows that have cues and no rate, from the
        // stick's metadata cache when it knows the file, probed once
        // otherwise and cached for the next pass. The Full stage needs no
        // source of its own: it always follows this one on the same rows.
#ifdef SEABASS_HAVE_TAGLIB
        infrastructure::audio::TagLibMetadataProbe probe;
#else
        application::NullTrackMetadataProbe probe;
#endif
        infrastructure::local::CachedSampleRates rates(infrastructure::paths::stickRootForCatalogPath(path), probe);
        engine->setSampleRateSource([&rates](const std::string &file) { return rates.rateOf(file); });
        try {
            engine->fillCues(tracks);
        } catch (...) {
            // What was probed before a cancel is still true.
            rates.save();
            throw;
        }
        // A read-only or full stick costs a probe next time, never the pass.
        rates.save();
        return;
    }
    case LibraryCatalogCache::Detail::Full:
        cancel.throwIfCancelled();
        // What the readers used to do inside every read and do not any
        // more: a stat per audio file for its size, and for Engine and
        // OneLibrary a stat per cover to drop the ones not on disk. The
        // same completeTracks() the CLI runs after its reads, so a page
        // and the command line agree on a catalog. Checked per file, so
        // a stick pulled mid pass (invalidateEveryCatalogOn() cancels the
        // prefetch) stops within one stat.
        {
            auto reader = makeReader(format, path);
            reader->setProgressReporter(progress);
            reader->setCancellationToken(cancel);
            if (auto *engine = dynamic_cast<infrastructure::engine::LibdjinteropEngineReader *>(reader.get())) {
                engine->setVolumeIdentity(volumeIdentityOf(path));
            }
            reader->fillArtwork(tracks);
        }
        application::completeTracks(tracks, cancel, progress);
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

std::optional<size_t> realCount(const std::string &format, const std::string &path)
{
    try {
        return makeReader(format, path)->countTracks();
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

// What realStage() announces for one stage of one catalog of `tracks`
// rows, in ticks. Kept next to realStage() so a stage that gains or
// loses an announcement changes this line too.
size_t unitsOfStage(LibraryCatalogCache::Detail stage, const std::string &format, size_t tracks)
{
    switch (stage) {
    case LibraryCatalogCache::Detail::Tracks:
        // "Scanning rekordbox tracks", "Scanning Engine tracks", "Reading
        // OneLibrary": one per row.
        return tracks;
    case LibraryCatalogCache::Detail::Cues:
        // "Reading rekordbox cues", "Reading OneLibrary cues": one per
        // row, the analysis files; Engine holds its cues in m.db and
        // announces nothing.
        return format == "rekordbox" || format == "onelibrary" ? tracks : 0;
    case LibraryCatalogCache::Detail::Full:
        // completeTracks(): "Checking files" per row, and for the two
        // catalogs that name cover files, "Checking cover files" per row.
        // The duration passes announce nothing.
        return format == "rekordbox" ? tracks : 2 * tracks;
    }
    return 0;
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

namespace
{
std::atomic<LibraryCatalogCache *> s_instanceForTesting{nullptr};
}

LibraryCatalogCache &LibraryCatalogCache::instance()
{
    if (LibraryCatalogCache *stand = s_instanceForTesting.load()) {
        return *stand;
    }
    static LibraryCatalogCache cache;
    return cache;
}

void LibraryCatalogCache::setInstanceForTesting(LibraryCatalogCache *cache)
{
    s_instanceForTesting.store(cache);
}

LibraryCatalogCache::LibraryCatalogCache() : m_stageFn(realStage), m_mtimeFn(realMtime), m_countFn(realCount)
{
    stopWhenTheProcessEnds();
}

LibraryCatalogCache::StageFn LibraryCatalogCache::realStageForTesting()
{
    return realStage;
}

LibraryCatalogCache::MtimeFn LibraryCatalogCache::realMtimeForTesting()
{
    return realMtime;
}

LibraryCatalogCache::LibraryCatalogCache(StageFn stageFn, MtimeFn mtimeFn)
    : m_stageFn(std::move(stageFn)), m_mtimeFn(std::move(mtimeFn)), m_countFn(realCount)
{
    stopWhenTheProcessEnds();
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
      m_mtimeFn(std::move(mtimeFn)), m_countFn(realCount)
{
    stopWhenTheProcessEnds();
}

bool LibraryCatalogCache::analysisCheckDueLocked(const Entry &entry) const
{
    return entry.analysisFiles && !entry.analysisFiles->empty()
        && m_nowFn() - entry.analysisCheckedAt >= analysisCheckWindow();
}

std::optional<std::uint64_t> LibraryCatalogCache::checkAnalysisFiles(const std::vector<std::string> &files,
                                                                    const application::CancellationToken &cancel)
{
    ++m_analysisChecks;
    return analysisStateOf(files, cancel);
}

void LibraryCatalogCache::noteAnalysisChecked(const std::string &key,
                                              const std::shared_ptr<const std::vector<std::string>> &files)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(key);
    // Only the entry whose files were checked: one read since has its own.
    if (it != m_entries.end() && it->second.analysisFiles == files) {
        it->second.analysisCheckedAt = m_nowFn();
    }
}

void LibraryCatalogCache::setNowFnForTesting(NowFn nowFn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nowFn = std::move(nowFn);
}

int LibraryCatalogCache::analysisChecksForTesting() const
{
    return m_analysisChecks.load();
}

void LibraryCatalogCache::setCountFnForTesting(CountFn countFn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_countFn = std::move(countFn);
    m_counts.clear();
}

std::optional<size_t> LibraryCatalogCache::countTracks(const std::string &format, const std::string &path,
                                                       application::CancellationToken cancel)
{
    const std::string key = keyFor(format, path);
    std::optional<std::chrono::system_clock::time_point> mtime;
    try {
        mtime = m_mtimeFn(format, path);
    } catch (const std::exception &) {
        // No catalog file to date the count by: count, remember nothing.
    }
    CountFn count;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        generation = m_generation[key];
        const auto found = m_counts.find(key);
        if (mtime && found != m_counts.end() && found->second.mtime == *mtime
            && found->second.generation == generation) {
            return found->second.rows;
        }
        count = m_countFn;
    }
    cancel.throwIfCancelled();
    const std::optional<size_t> rows = count(format, path);
    if (mtime) {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Not when an invalidation landed during the count: it may have
        // counted the catalog from before the write.
        if (m_generation[key] == generation) {
            m_counts[key] = RememberedCount{*mtime, generation, rows};
        }
    }
    return rows;
}

std::optional<size_t> LibraryCatalogCache::plannedUnits(const std::string &format, const std::string &path,
                                                        Detail detail, application::CancellationToken cancel)
{
    const int wanted = stageNumber(detail);
    const std::string key = keyFor(format, path);
    std::chrono::system_clock::time_point currentMtime;
    try {
        currentMtime = m_mtimeFn(format, path);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    int have = 0;
    std::shared_ptr<const std::vector<std::string>> analysisFiles;
    std::uint64_t analysisState = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it != m_entries.end() && it->second.mtime == currentMtime) {
            // A pass another thread is in is planned like one still to run:
            // tracksFor() waits for it and shows its progress on the
            // caller's bar as it goes (#67), so the bar neither stalls
            // through the wait nor runs past a plan that left it out.
            have = it->second.stage;
            // A Tracks request is served without the check (see
            // stagedTracksFor()), and one made lately stands.
            if (wanted > stageNumber(Detail::Tracks) && analysisCheckDueLocked(it->second)) {
                analysisFiles = it->second.analysisFiles;
                analysisState = it->second.analysisState;
            }
        }
    }
    if (analysisFiles) {
        const auto state = checkAnalysisFiles(*analysisFiles, cancel);
        if (state && *state != analysisState) {
            // A player rewrote an analysis file: tracksFor() reads it all
            // again.
            have = 0;
        } else if (state) {
            noteAnalysisChecked(key, analysisFiles);
        }
    }
    if (have >= wanted) {
        return 0;
    }
    const std::optional<size_t> tracks = countTracks(format, path, std::move(cancel));
    if (!tracks) {
        return std::nullopt;
    }
    size_t units = 0;
    for (int stage = have + 1; stage <= wanted; ++stage) {
        units += unitsOfStage(detailOf(stage), format, *tracks);
    }
    return units;
}

// A pass in progress when the process begins to end is cancelled there
// and then, so it ends inside endProcess()'s bound for reads instead of
// running on until the bound cuts the process off without its static
// destructors (a real stick's cue stage alone takes seconds).
void LibraryCatalogCache::stopWhenTheProcessEnds()
{
    RunningReads::instance().onShutdown(this, [this] { stopPrefetching(); });
}

void LibraryCatalogCache::stopPrefetching()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        m_prefetchQueue.clear();
        m_prefetchCancel.cancel();
    }
    m_prefetchCv.notify_all();
}

LibraryCatalogCache::~LibraryCatalogCache()
{
    RunningReads::instance().removeShutdownHook(this);
    stopPrefetching();
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
    // The analysis files' state, checked against the entry's whenever the
    // entry holds cues read from them; taken outside the lock (a stat per
    // file), so the loop takes it again if the entry changed meanwhile.
    //
    // Not for every request: at most once per entry per
    // analysisCheckWindow(), so one operation's burst of plans and reads
    // pays it once, and never for a request for Tracks alone, whose rows
    // no analysis file changes. Such a request served from an entry that
    // holds cues not checked lately is told it has Tracks only, so a
    // caller that wants cues asks for them and gets the check. A stop
    // during the check leaves the entry as it is for this call.
    std::shared_ptr<const std::vector<std::string>> checkedFiles;
    std::optional<std::uint64_t> checkedState;
    // The other thread's pass this call has shown so far, and how far.
    struct
    {
        std::shared_ptr<PassProgress> pass;
        std::uint64_t stretches = 0;
        size_t current = 0;
    } mirrored;
    for (;;) {
        Entry &entry = m_entries[key];
        bool fresh = entry.stage > 0 && entry.mtime == currentMtime;
        bool cuesVouchedFor = true;
        if (fresh && analysisCheckDueLocked(entry)) {
            if (checkedFiles == entry.analysisFiles) {
                // Checked in this call.
                if (checkedState && *checkedState != entry.analysisState) {
                    fresh = false;
                } else if (checkedState) {
                    entry.analysisCheckedAt = m_nowFn();
                }
            } else if (wanted <= stageNumber(Detail::Tracks)) {
                cuesVouchedFor = false;
            } else {
                const auto files = entry.analysisFiles;
                lock.unlock();
                const auto state = checkAnalysisFiles(*files, cancel);
                lock.lock();
                checkedFiles = files;
                checkedState = state;
                continue;
            }
        }
        if (fresh && entry.stage >= wanted) {
            // The stage with the tracks, under this one lock.
            return {entry.tracks,
                    cuesVouchedFor ? detailOf(entry.stage) : std::min(detailOf(entry.stage), Detail::Tracks)};
        }
        if (entry.passInFlight == 0) {
            if (!fresh) {
                // Nothing yet, or read from a catalog file that has changed
                // since: start over from the catalog.
                entry.tracks.clear();
                entry.notes = {};
                entry.stage = 0;
                entry.mtime = currentMtime;
                entry.analysisFiles = nullptr;
                entry.analysisState = 0;
                entry.analysisCheckedAt = {};
            }
            break;
        }
        // Woken by the pass finishing, an invalidation, or the caller's
        // own token: a page left mid-wait must not keep a worker thread
        // parked behind a cue pass that takes 8 s on a cold stick.
        ++m_waiting;
        m_cv.wait_for(lock, std::chrono::milliseconds(100));
        --m_waiting;
        // The pass being waited for, shown on this caller's bar as far as
        // it has got (#67): its stretch as it begins one, then its ticks.
        if (std::shared_ptr<PassProgress> pass = m_entries[key].passProgress) {
            std::string label;
            size_t total = 0;
            size_t current = 0;
            std::uint64_t stretches = 0;
            {
                std::lock_guard<std::mutex> passLock(pass->mutex);
                label = pass->label;
                total = pass->total;
                current = pass->current;
                stretches = pass->stretches;
            }
            if (stretches > 0) {
                lock.unlock();
                if (pass != mirrored.pass || stretches != mirrored.stretches) {
                    progress.start(label, total);
                    mirrored = {pass, stretches, 0};
                }
                if (current > mirrored.current) {
                    progress.tick(current);
                    mirrored.current = current;
                }
                lock.lock();
            }
        }
        // The files may have moved while this caller waited.
        checkedFiles = nullptr;
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
        std::shared_ptr<PassProgress> passProgress;
        if (committing) {
            m_entries[key].passInFlight = next;
            passProgress = std::make_shared<PassProgress>();
            m_entries[key].passProgress = passProgress;
        }
        lock.unlock();

        std::shared_ptr<const std::vector<std::string>> analysisFiles;
        std::uint64_t analysisState = 0;
        std::chrono::steady_clock::time_point analysisTakenAt;
        std::exception_ptr error;
        try {
            cancel.throwIfCancelled();
            // Before the cue pass reads them, so a file rewritten while it
            // runs leaves the entry stale rather than fresh with the old
            // cues. Inside the try: a path that does not convert must end
            // the pass like any reader error, not leave it claimed.
            if (detailOf(next) == Detail::Cues) {
                analysisFiles = analysisFilesOf(format, path, work);
                if (analysisFiles) {
                    const auto state = analysisStateOf(*analysisFiles, cancel);
                    if (!state) {
                        throw application::OperationCancelled();
                    }
                    analysisState = *state;
                }
                analysisTakenAt = m_nowFn();
            }
            if (passProgress) {
                RecordingReporter recording(progress, *passProgress);
                m_stageFn(detailOf(next), format, path, work, notes, recording, cancel);
            } else {
                m_stageFn(detailOf(next), format, path, work, notes, progress, cancel);
            }
        } catch (...) {
            error = std::current_exception();
        }

        lock.lock();
        if (committing) {
            if (m_generation[key] == generationAtStart) {
                Entry &entry = m_entries[key];
                entry.passInFlight = 0;
                entry.passProgress = nullptr;
                if (!error) {
                    // Committed and the next pass claimed (at the top of
                    // the loop) under one lock, so a caller waiting for
                    // a later stage cannot slip in and read it twice.
                    entry.tracks = work;
                    entry.notes = notes;
                    entry.stage = next;
                    entry.mtime = currentMtime;
                    if (detailOf(next) == Detail::Cues) {
                        entry.analysisFiles = analysisFiles;
                        entry.analysisState = analysisState;
                        entry.analysisCheckedAt = analysisTakenAt;
                    }
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
        // A pass is a read like any page's, and the end of the process has
        // to know it is running: static destructors under it (the null
        // reporter it reports to, the readers' statics) are a use after
        // destroy. Once the process is ending, no pass starts at all.
        if (!RunningReads::instance().enterUnlessEnding()) {
            m_prefetchQueue.clear();
            m_prefetchCv.notify_all();
            continue;
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
        RunningReads::instance().leave();

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
    m_counts.erase(key);
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
    // Both ways: the two share their cues' analysis files (#59), so a
    // OneLibrary cue write changes what DeviceLibrary reads as well.
    if (format == "rekordbox") {
        invalidate("onelibrary", path);
    } else if (format == "onelibrary") {
        invalidate("rekordbox", path);
    }
}

}  // namespace seabass::gui
