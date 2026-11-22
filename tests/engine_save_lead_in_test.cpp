// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// #50 through a GUI save: the Engine cue writer a save builds answers a
// file's lead-in from the same checked metadata cache the catalog read
// the cues through, so a cue the save did not touch is written back
// exactly where it was.
//
// The stick's cache holds a lead-in for the file at its current size and
// mtime that is not what the file's start says (an FFmpeg one, 25.06 ms,
// on a LAME file, 51.18 ms). The catalog reads the track's hot cue
// through it; Add Cue then rewrites the track's whole cue set. A writer
// reading the file's start on its own takes 51.18 ms off a cue the
// reader added 25.06 ms to, and the untouched cue moves 26 ms early.
//
// And Restore Metadata (#50): cues the metadata store took in from Engine
// carry the shift the reader added then (0 for a store written before it
// recorded one: Engine's own positions). Restored to Engine they land on
// the sample they were read from, whatever the correction setting is now.
// Cues read from a row Engine's rekordbox import left unplayed are on
// rekordbox's timeline: stored as such, restored to Engine the file's
// shift earlier with the mark gone, and to rekordbox as they are.
//
// Usage: engine_save_lead_in_test FIXTURE_DIR (tests/fixtures/mp3-lead-in)

#include <QString>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/v2/engine_library.hpp>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/matching_policy.hpp"
#include "domain/track.hpp"
#include "domain/metadata_backup_plan.hpp"
#include "domain/metadata_restore.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/changes/restore_metadata_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/local/metadata_store.hpp"
#include "infrastructure/local/metadata_cache.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using seabass::domain::CuePoint;
using seabass::domain::Track;
namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    if (argc < 2 || !fs::is_directory(argv[1])) {
        std::cerr << "usage: engine_save_lead_in_test FIXTURE_DIR (tests/fixtures/mp3-lead-in)\n";
        return 2;
    }
    const fs::path fixtures = seabass::pathFromUtf8(argv[1]);
    seabass::domain::MatchingPolicy::reset();
    assert(seabass::domain::MatchingPolicy::correctMp3CueTiming());

    const fs::path stick = seabass::testing::scratchRoot() / "seabass_engine_save_lead_in_test";
    fs::remove_all(stick);
    const fs::path library = stick / "Engine Library";
    fs::create_directories(library);
    const fs::path file = library / "song.mp3";
    fs::copy_file(fixtures / "lame-cbr-44100.mp3", file);

    // A hot cue at 5000 ms, stored by Create Engine Library with the
    // file's own 51.18 ms: (5000 - 51.18) * 44.1 samples.
    Track t;
    t.sourceId = "r0";
    t.title = "Lead In";
    t.artist = "Artist";
    t.filePath = seabass::pathToUtf8(file);
    t.bpm = 120.0;
    t.durationSeconds = 300.0;
    t.cues = {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#ff0000", "drop"}};
    const auto created = seabass::infrastructure::engine::EngineLibraryCreator::create(
        seabass::pathToUtf8(library), {t}, seabass::infrastructure::engine::EngineSchemaGeneration::V2);
    assert(created.errorMessage.empty() && created.tracksCreated == 1);

    auto rawHotCue = [&](int pad) {
        auto db = djinterop::engine::load_database(seabass::pathToUtf8(library));
        const auto tracks = db.tracks();
        assert(tracks.size() == 1);
        const auto hot = tracks.front().hot_cues();
        assert(hot.size() >= static_cast<std::size_t>(pad) && hot[pad - 1]);
        return std::make_pair(std::to_string(tracks.front().id()), hot[pad - 1]->sample_offset);
    };
    const auto [sourceId, before] = rawHotCue(1);

    // The stick's metadata cache says the file is an FFmpeg one.
    {
        const auto lavc =
            seabass::infrastructure::audio::readMp3LeadIn(seabass::pathToUtf8(fixtures / "ffmpeg-lavc-44100.mp3"));
        assert(lavc && lavc->tag == seabass::domain::Mp3LeadIn::Tag::Lavc);
        seabass::infrastructure::local::MetadataCache cache(seabass::pathToUtf8(stick));
        seabass::application::FileMetadata entry;
        entry.durationSeconds = 3.0;
        entry.sampleRate = 44100;
        entry.mp3LeadInRead = true;
        entry.mp3LeadIn = lavc;
        cache.store(seabass::pathToUtf8(file), entry);
        assert(cache.save());
    }

    auto &noProgress = seabass::application::NullProgressReporter::instance();
    seabass::application::CancellationToken token;
    const QString root = pathToQString(library);
    // The save's context ends with the save, as in the app: it keeps the
    // stick's log open, and Windows will not delete the stick under it.
    const SaveLoopResult result = [&] {
        SaveContext ctx(token, noProgress, {}, {}, root);
        std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<AddCueChange>(
            QStringLiteral("engine"), root, QString::fromStdString(sourceId), 1234.0, QStringLiteral("hot"), 2,
            QStringLiteral("#00FF00"), QString(), false, 0.0, QStringLiteral("Lead In"))};
        return runSaveLoop(changes, ctx);
    }();
    if (!result.error.isEmpty()) {
        std::cerr << "save failed: " << result.error.toStdString() << "\n";
    }
    assert(result.error.isEmpty() && result.appliedIds.size() == 1);

    const auto [sameId, after] = rawHotCue(1);
    std::cout << "hot cue 1 before " << before << ", after " << after << " samples\n";
    assert(std::abs(after - before) < 1e-3 && "the cue the save did not touch stays where it was");
    // And the new one is moved by the cache's 1105 samples too.
    const auto [newId, added] = rawHotCue(2);
    assert(std::abs(added - (1234.0 * 44.1 - 1105.0)) < 1e-3);
    (void)sameId;
    (void)newId;
    assert(result.warning.isEmpty() && "nothing was left out");

    // A memory cue and a loop inside the lead-in the cache answers
    // (25.06 ms): the writer leaves both out, and the save says so in its
    // outcome rather than reporting them written.
    {
        SaveContext leftOutCtx(token, noProgress, {}, {}, root);
        std::vector<std::shared_ptr<PendingChange>> inLeadIn = {
            std::make_shared<AddCueChange>(QStringLiteral("engine"), root, QString::fromStdString(sourceId), 10.0,
                                           QStringLiteral("memory"), 0, QString(), QString(), false, 0.0,
                                           QStringLiteral("Lead In")),
            std::make_shared<AddCueChange>(QStringLiteral("engine"), root, QString::fromStdString(sourceId), 5.0,
                                           QStringLiteral("hot"), 3, QStringLiteral("#00FF00"), QString(), true, 20.0,
                                           QStringLiteral("Lead In"))};
        const SaveLoopResult leftOut = runSaveLoop(inLeadIn, leftOutCtx);
        std::cout << "left out: error \"" << leftOut.error.toStdString() << "\", warning \""
                  << leftOut.warning.toStdString() << "\"\n";
        assert(leftOut.error.isEmpty() && leftOut.appliedIds.size() == 2);
        assert(leftOut.warning.contains(QStringLiteral("1 memory cue and 1 loop inside the MP3 lead-in were left out"))
               && "the dropped cues reach the save's outcome");
    }

    fs::remove_all(stick);

    // Restore Metadata onto a fresh library with the same file and no
    // metadata cache: the writer reads the file's own 51.18 ms.
    const fs::path restoreStick = seabass::testing::scratchRoot() / "seabass_engine_save_lead_in_test_restore";
    fs::remove_all(restoreStick);
    const fs::path restoreLibrary = restoreStick / "Engine Library";
    fs::create_directories(restoreLibrary);
    const fs::path restoreFile = restoreLibrary / "song.mp3";
    fs::copy_file(fixtures / "lame-cbr-44100.mp3", restoreFile);
    Track bare = t;
    bare.filePath = seabass::pathToUtf8(restoreFile);
    bare.cues.clear();
    const auto restoreCreated = seabass::infrastructure::engine::EngineLibraryCreator::create(
        seabass::pathToUtf8(restoreLibrary), {bare}, seabass::infrastructure::engine::EngineSchemaGeneration::V2);
    assert(restoreCreated.errorMessage.empty() && restoreCreated.tracksCreated == 1);
    const std::string restoreId = [&] {
        auto db = djinterop::engine::load_database(seabass::pathToUtf8(restoreLibrary));
        return std::to_string(db.tracks().front().id());
    }();
    const double shiftMs = 2257 * 1000.0 / 44100;

    // Restores hot cue 1 at `positionMs` carrying `storedShift`, and
    // answers the sample Engine stores it at.
    auto restore = [&](double positionMs, std::optional<double> storedShift) {
        seabass::domain::MetadataRestoreProposal proposal;
        proposal.stickTrack = bare;
        proposal.stickTrack.sourceId = restoreId;
        proposal.cuesOffered = true;
        proposal.cues = {CuePoint{CuePoint::Kind::Hot, 1, positionMs, "#ff0000", "drop"}};
        proposal.cuesEngineShiftMs = storedShift;
        seabass::application::CancellationToken restoreToken;
        const QString restoreRoot = pathToQString(restoreLibrary);
        SaveContext restoreCtx(restoreToken, noProgress, {}, {}, restoreRoot);
        std::vector<std::shared_ptr<PendingChange>> restoreChanges = {std::make_shared<RestoreMetadataChange>(
            QStringLiteral("engine"), restoreRoot, QString::fromStdString(restoreId), proposal)};
        const SaveLoopResult restored = runSaveLoop(restoreChanges, restoreCtx);
        if (!restored.error.isEmpty()) {
            std::cerr << "restore failed: " << restored.error.toStdString() << "\n";
        }
        assert(restored.error.isEmpty() && restored.appliedIds.size() == 1);
        auto db = djinterop::engine::load_database(seabass::pathToUtf8(restoreLibrary));
        const auto hot = db.tracks().front().hot_cues();
        assert(!hot.empty() && hot[0]);
        return hot[0]->sample_offset;
    };
    const double original = 5000.0 * 44.1;  // the Engine sample the cue was read from

    // A row stored before the store recorded a shift: Engine's own
    // position. With the correction on, the writer takes 51.18 ms off
    // whatever it is given, so the restore adds it first.
    {
        const double stored = restore(5000.0, 0.0);
        std::cout << "old-schema row, correction on: " << stored << " (want " << original << ")\n";
        assert(std::abs(stored - original) < 1e-3);
    }
    // A row read with the shift on, restored with the correction off: the
    // writer takes nothing off, so the restore does.
    {
        seabass::domain::MatchingPolicy::setCorrectMp3CueTiming(false);
        const double stored = restore(5000.0 + shiftMs, shiftMs);
        seabass::domain::MatchingPolicy::setCorrectMp3CueTiming(true);
        std::cout << "row stored with " << shiftMs << " ms, correction off: " << stored << " (want " << original
                  << ")\n";
        assert(std::abs(stored - original) < 1e-3);
    }
    // The same row with the correction on, and cues on rekordbox's
    // timeline (from a rekordbox stick): both moved by the writer alone.
    {
        assert(std::abs(restore(5000.0 + shiftMs, shiftMs) - original) < 1e-3);
        assert(std::abs(restore(5000.0, std::nullopt) - (original - 2257.0)) < 1e-3);
    }

    // Restored to rekordbox: the Engine position plus the shift rekordbox
    // is ahead by today, which for a row stored with the shift on is the
    // position as stored. What the change writes is what rekordboxWrites()
    // reports, so that is checked without a rekordbox catalog.
    {
        auto pioneerCues = [&](double positionMs, std::optional<double> storedShift) {
            seabass::domain::MetadataRestoreProposal proposal;
            proposal.stickTrack = bare;
            proposal.stickTrack.sourceId = "1";
            proposal.cuesOffered = true;
            proposal.cues = {CuePoint{CuePoint::Kind::Hot, 1, positionMs, "#ff0000", "drop"}};
            proposal.cuesEngineShiftMs = storedShift;
            RestoreMetadataChange change(QStringLiteral("rekordbox"), QStringLiteral("/nowhere/PIONEER"),
                                         QStringLiteral("1"), proposal);
            const auto writes = change.rekordboxWrites();
            assert(writes.size() == 1 && writes.front().cues && writes.front().cues->size() == 1);
            return writes.front().cues->front().positionMs;
        };
        assert(std::abs(pioneerCues(5000.0, 0.0) - (5000.0 + shiftMs)) < 1e-9 && "Engine's own position, moved");
        assert(std::abs(pioneerCues(5000.0 + shiftMs, shiftMs) - (5000.0 + shiftMs)) < 1e-9);
        assert(pioneerCues(5000.0, std::nullopt) == 5000.0 && "rekordbox's timeline already");
        seabass::domain::MatchingPolicy::setCorrectMp3CueTiming(false);
        assert(std::abs(pioneerCues(5000.0 + shiftMs, shiftMs) - 5000.0) < 1e-9 && "the setting off: Engine's");
        seabass::domain::MatchingPolicy::setCorrectMp3CueTiming(true);
    }

    // A row Engine's rekordbox import left unplayed (thirdPartySourceId
    // 1), captured into the metadata store and restored. Its hot cue is
    // stored at rekordbox's 5000 ms (220500 samples), unmoved, as that
    // import leaves it. Read as stored, it goes into the store as on
    // rekordbox's timeline (no Engine shift); restored to Engine, the
    // writer takes the file's 51.18 ms off and the mark goes, and to
    // rekordbox it is written as it is.
    {
        const fs::path markedStick = seabass::testing::scratchRoot() / "seabass_engine_save_lead_in_test_marked";
        fs::remove_all(markedStick);
        const fs::path markedLibrary = markedStick / "Engine Library";
        fs::create_directories(markedLibrary);
        const fs::path markedFile = markedLibrary / "song.mp3";
        fs::copy_file(fixtures / "lame-cbr-44100.mp3", markedFile);
        Track song = bare;
        song.filePath = seabass::pathToUtf8(markedFile);
        const auto created = seabass::infrastructure::engine::EngineSchemaGeneration::V2;
        const auto made = seabass::infrastructure::engine::EngineLibraryCreator::create(
            seabass::pathToUtf8(markedLibrary), {song}, created);
        assert(made.errorMessage.empty() && made.tracksCreated == 1);
        const std::string libraryPath = seabass::pathToUtf8(markedLibrary);
        const std::int64_t rowId = [&] {
            auto db = djinterop::engine::load_database(libraryPath);
            return db.tracks().front().id();
        }();
        const std::string id = std::to_string(rowId);
        auto setMark = [&](std::optional<std::int64_t> value) {
            djinterop::engine::v2::engine_library::load(libraryPath).track().set_third_party_source_id(rowId, value);
        };
        auto markOf = [&] {
            return djinterop::engine::v2::engine_library::load(libraryPath).track().get_third_party_source_id(rowId);
        };
        auto storedHotCue = [&] {
            auto db = djinterop::engine::load_database(libraryPath);
            const auto hot = db.tracks().front().hot_cues();
            assert(!hot.empty() && hot[0]);
            return hot[0]->sample_offset;
        };
        // Written as the import leaves it: no shift taken off, then marked.
        auto writeUnmoved = [&](const std::vector<CuePoint> &cues) {
            seabass::infrastructure::engine::LibdjinteropEngineCueWriter raw(libraryPath);
            raw.setLeadInSource([](const std::string &) { return std::optional<seabass::domain::Mp3LeadIn>(); });
            raw.writeHotCues(id, cues);
            setMark(1);
        };
        writeUnmoved({CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#ff0000", "drop"}});
        assert(std::abs(storedHotCue() - original) < 1e-3 && markOf() == 1);

        auto readStick = [&] {
            seabass::infrastructure::engine::LibdjinteropEngineReader reader(libraryPath);
            auto tracks = reader.readTracks();
            assert(tracks.size() == 1);
            return tracks.front();
        };
        const Track captured = readStick();
        assert(captured.cues.size() == 1 && std::abs(captured.cues[0].positionMs - 5000.0) < 1e-6);
        assert(captured.cueTimelineShiftMs == 0.0 && captured.engineCuesOnRekordboxTimeline);

        const fs::path storePath = markedStick / "local" / "metadata.db";
        std::vector<Track> stored;
        {
            seabass::infrastructure::local::MetadataStore store(storePath);
            seabass::infrastructure::local::MetadataSource source;
            source.stickRoot = markedStick;
            source.libraryId = "library-marked";
            source.stickLabel = "MARKED";
            source.catalogModifiedAt = 1'700'000'000;
            store.store({captured}, source, noProgress, seabass::application::CancellationToken::none());
            stored = store.readAll();
        }
        assert(stored.size() == 1 && stored.front().cues.size() == 1);
        std::cout << "marked row stored with shift "
                  << (stored.front().storedEngineCueShiftMs ? std::to_string(*stored.front().storedEngineCueShiftMs)
                                                            : std::string("none"))
                  << "\n";
        assert(!stored.front().storedEngineCueShiftMs.has_value() && "on rekordbox's timeline, like rekordbox's own");

        // The same row in a store an earlier build filled: it added the
        // file's shift to the cue and recorded it. Taken in again as read
        // now, the stored cue is put at its place on rekordbox's timeline
        // with no shift, and the restore below is made from that store.
        {
            seabass::infrastructure::local::MetadataStore earlier(markedStick / "local" / "metadata-earlier.db");
            seabass::infrastructure::local::MetadataSource source;
            source.stickRoot = markedStick;
            source.libraryId = "library-marked";
            source.stickLabel = "MARKED";
            source.catalogModifiedAt = 1'700'000'000;
            Track misread = captured;
            misread.engineCuesOnRekordboxTimeline = false;
            misread.cueTimelineShiftMs = shiftMs;
            misread.cues[0].positionMs += shiftMs;
            earlier.store({misread}, source, noProgress, seabass::application::CancellationToken::none());
            const auto before = earlier.readAll();
            assert(before.size() == 1 && before.front().storedEngineCueShiftMs
                   && std::abs(*before.front().storedEngineCueShiftMs - shiftMs) < 1e-9);
            source.catalogModifiedAt = 1'700'000'100;
            // As the backup page does it: the plan offers the track, and
            // what it offers is stored.
            const auto plan = seabass::domain::planMetadataBackup({captured}, before, source.catalogModifiedAt);
            assert(plan.proposals.size() == 1 && plan.proposals.front().cueTimingCorrected);
            earlier.store({plan.proposals.front().stickTrack}, source, noProgress,
                          seabass::application::CancellationToken::none());
            stored = earlier.readAll();
        }
        assert(stored.size() == 1 && stored.front().cues.size() == 1);
        std::cout << "earlier store, retaken: cue at " << stored.front().cues[0].positionMs << " ms, shift "
                  << (stored.front().storedEngineCueShiftMs ? std::to_string(*stored.front().storedEngineCueShiftMs)
                                                            : std::string("none"))
                  << "\n";
        assert(std::abs(stored.front().cues[0].positionMs - 5000.0) < 1e-9);
        assert(!stored.front().storedEngineCueShiftMs.has_value());

        // The stick loses the cue (the row still marked) and the store
        // offers it back.
        writeUnmoved({});
        Track stickNow = readStick();
        assert(stickNow.cues.empty());
        auto proposals = seabass::domain::planMetadataRestore({stickNow}, stored, 0);
        assert(proposals.size() == 1 && proposals.front().cuesOffered);
        assert(!proposals.front().cuesEngineShiftMs.has_value());

        seabass::application::CancellationToken markedToken;
        const QString markedRoot = pathToQString(markedLibrary);
        {
            SaveContext markedCtx(markedToken, noProgress, {}, {}, markedRoot);
            std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<RestoreMetadataChange>(
                QStringLiteral("engine"), markedRoot, QString::fromStdString(id), proposals.front())};
            const SaveLoopResult restored = runSaveLoop(changes, markedCtx);
            if (!restored.error.isEmpty()) {
                std::cerr << "restore failed: " << restored.error.toStdString() << "\n";
            }
            assert(restored.error.isEmpty() && restored.appliedIds.size() == 1);
        }
        std::cout << "marked row restored to Engine: " << storedHotCue() << " (want " << original - 2257.0
                  << "), mark " << (markOf() ? std::to_string(*markOf()) : std::string("NULL")) << "\n";
        assert(std::abs(storedHotCue() - (original - 2257.0)) < 1e-3);
        assert(!markOf().has_value());
        const Track afterRestore = readStick();
        assert(std::abs(afterRestore.cues[0].positionMs - 5000.0) < 1e-6 && !afterRestore.engineCuesOnRekordboxTimeline);

        seabass::domain::MetadataRestoreProposal toPioneer = proposals.front();
        toPioneer.stickTrack.sourceId = "1";
        RestoreMetadataChange pioneer(QStringLiteral("rekordbox"), QStringLiteral("/nowhere/PIONEER"),
                                      QStringLiteral("1"), toPioneer);
        const auto writes = pioneer.rekordboxWrites();
        assert(writes.size() == 1 && writes.front().cues && writes.front().cues->size() == 1);
        std::cout << "marked row restored to rekordbox: " << writes.front().cues->front().positionMs << " ms\n";
        assert(std::abs(writes.front().cues->front().positionMs - 5000.0) < 1e-9);

        // A stored copy taken from Engine (it carries the shift it was
        // read with) restored onto a row that carries the mark: put back
        // as Engine had it, the shift taken off, and the mark left. Whose
        // timeline such a copy is on cannot be told.
        writeUnmoved({});
        assert(markOf() == 1);
        seabass::domain::MetadataRestoreProposal fromEngine = proposals.front();
        fromEngine.cues = {CuePoint{CuePoint::Kind::Hot, 1, 5000.0 + shiftMs, "#ff0000", "drop"}};
        fromEngine.cuesEngineShiftMs = shiftMs;
        {
            SaveContext engineCtx(markedToken, noProgress, {}, {}, markedRoot);
            std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<RestoreMetadataChange>(
                QStringLiteral("engine"), markedRoot, QString::fromStdString(id), fromEngine)};
            const SaveLoopResult restored = runSaveLoop(changes, engineCtx);
            assert(restored.error.isEmpty() && restored.appliedIds.size() == 1);
        }
        std::cout << "Engine-origin copy restored onto a marked row: " << storedHotCue() << " (want " << original
                  << "), mark " << (markOf() ? std::to_string(*markOf()) : std::string("NULL")) << "\n";
        assert(std::abs(storedHotCue() - original) < 1e-3 && "stored minus the stored shift");
        assert(markOf() == 1 && "the mark left as it was");
        fs::remove_all(markedStick);
    }

    fs::remove_all(restoreStick);
    seabass::domain::MatchingPolicy::reset();
    std::cout << "engine_save_lead_in_test passed\n";
    return 0;
}
