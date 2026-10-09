// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// RecordRekordboxBaselineChange: Sync after Rekordbox Export's record of
// the rekordbox side, made after the commit of the page's save, on a copy
// of the anonymized fixture with both catalogs and the import counter
// levelled (export.pdb at 15132).
//
// 1. A first save records the baseline it was given, at export.pdb's
//    sequence after the save, readable; Undo takes the file away again.
// 2. Over an earlier baseline, Undo puts that one back byte for byte.
// 3. A save that stops at a failing change records the items of the
//    changes that landed and keeps the previous value of the others; a
//    save where nothing landed records nothing.
// 4. A wrapped SyncPlanChange onto rekordbox is in the ledger: the
//    track's cues are the written set, of Seabass origin.

#include <QCoreApplication>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

#include "gui/edit/changes/owned_change.hpp"
#include "gui/edit/changes/record_rekordbox_baseline_change.hpp"
#include "rekordbox_baseline_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
using domain::CuePoint;
using domain::ValueOrigin;
namespace fs = std::filesystem;

namespace
{

std::shared_ptr<PendingChange> owned(std::unique_ptr<PendingChange> change)
{
    return std::shared_ptr<PendingChange>(ownedByRekordboxExportSync(std::move(change)));
}

std::shared_ptr<PendingChange> record(const testing::EngineChangeStick &stick, domain::RekordboxBaseline next,
                                      RecordRekordboxBaselineChange::AppliedBy appliedBy)
{
    return std::make_shared<RecordRekordboxBaselineChange>(testing::stickRootOf(stick), std::move(next),
                                                           std::move(appliedBy));
}

void reportError(const SaveLoopResult &result, const char *what)
{
    if (!result.error.isEmpty()) {
        std::cerr << what << ": " << result.error.toStdString() << "\n";
    }
    if (!result.warning.isEmpty()) {
        std::cerr << what << " (warning): " << result.warning.toStdString() << "\n";
    }
}

const QString SyncOntoEngineId = QStringLiteral("rekordbox-export-sync:sync:engine:6");

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: record_rekordbox_baseline_change_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    const fs::path scratch = testing::scratchRoot() / "record_rekordbox_baseline_change_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    testing::sandboxSeabassHome(scratch / "home");
    testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    // 1. The first save on a stick: the baseline as given, at the sequence
    //    the save left, readable; Undo removes it with m.db put back.
    {
        const auto stick = testing::makeEngineChangeStick(fixture, "record_first");
        testing::levelImportCounter(stick);
        const auto tracks = testing::rekordboxTracks(stick, 2);
        const std::string k1 = testing::keyOf(stick, tracks[0]);
        const std::string k2 = testing::keyOf(stick, tracks[1]);
        assert(!fs::exists(testing::baselineFileOf(stick)) && "the precondition: no baseline yet");
        const std::string dbBefore = testing::fileBytes(pathFromUtf8(stick.db));

        domain::RekordboxBaseline next;
        next.engineUuid = "uuid-test";
        next.tracks = {testing::baselineRow(stick, tracks[0], 3, ValueOrigin::Rekordbox, 5000.0),
                       testing::baselineRow(stick, tracks[1], std::nullopt, ValueOrigin::Unknown, 6000.0)};
        next.playlists = {domain::BaselinePlaylist{7, 0, false, "Set", {k1, k2}}};
        next.declined = {{"rating:" + k2, "abc"}};
        const SaveLoopResult saved = testing::saveChanges(
            {owned(testing::syncOntoEngine(stick)), record(stick, next, {{SyncOntoEngineId.toStdString(), {"cue:hot:1:" + k1}}})},
            stick.pioneerPath(), stick.enginePath());
        reportError(saved, "save");
        assert(saved.error.isEmpty() && saved.warning.isEmpty());
        assert((saved.appliedIds == QStringList{SyncOntoEngineId, RecordRekordboxBaselineChange::idString()}));
        assert(RecordRekordboxBaselineChange::idString() == QStringLiteral("rekordbox-export-sync:baseline"));

        assert(fs::exists(testing::baselineFileOf(stick)));
        const auto recorded = testing::readBaseline(stick);
        assert(recorded);
        assert(testing::pdbSequence(stick) == testing::FixtureSequence && "an Engine-only save leaves export.pdb");
        assert(recorded->pdbSequence == testing::FixtureSequence);
        assert(recorded->engineUuid == "uuid-test");
        assert(recorded->writer.rfind("Seabass ", 0) == 0 && recorded->recordedAtUnix > 0);
        assert(recorded->tracks.size() == 2);
        assert(recorded->tracks[0].pathKey == k1 && recorded->tracks[0].rating == 3
               && recorded->tracks[0].ratingOrigin == ValueOrigin::Rekordbox);
        assert(recorded->tracks[0].cues.size() == 1 && recorded->tracks[0].cues[0].cue.positionMs == 5000.0
               && recorded->tracks[0].cues[0].origin == ValueOrigin::Rekordbox);
        assert(recorded->tracks[1].pathKey == k2 && !recorded->tracks[1].rating);
        assert(recorded->playlists.size() == 1 && recorded->playlists[0].id == 7 && recorded->playlists[0].path == "Set");
        assert((recorded->playlists[0].members == std::vector<std::string>{k1, k2}));
        assert((recorded->declined == std::map<std::string, std::string>{{"rating:" + k2, "abc"}}));
        assert(testing::fileBytes(pathFromUtf8(stick.db)) != dbBefore);

        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        reportError(undone, "undo");
        assert(undone.error.isEmpty() && undone.appliedIds.size() == 1);
        assert(!fs::exists(testing::baselineFileOf(stick)) && "Undo takes away the baseline the save created");
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == dbBefore);
        std::cout << "case 1 (first save records the baseline at 15132; Undo removes it) OK\n";
    }

    // 2. Over an earlier baseline: recorded anew, and Undo puts the earlier
    //    one back byte for byte.
    {
        const auto stick = testing::makeEngineChangeStick(fixture, "record_over_previous");
        testing::levelImportCounter(stick);
        const auto tracks = testing::rekordboxTracks(stick, 1);
        domain::RekordboxBaseline previous;
        previous.pdbSequence = 100;
        previous.tracks = {testing::baselineRow(stick, tracks[0], 1, ValueOrigin::Rekordbox, 4000.0)};
        testing::plantBaseline(stick, previous);
        const std::string planted = testing::fileBytes(testing::baselineFileOf(stick));

        domain::RekordboxBaseline next;
        next.tracks = {testing::baselineRow(stick, tracks[0], 5, ValueOrigin::Unknown, 4000.0)};
        const SaveLoopResult saved = testing::saveChanges(
            {owned(testing::syncOntoEngine(stick)), record(stick, next, {})}, stick.pioneerPath(), stick.enginePath());
        reportError(saved, "save");
        assert(saved.error.isEmpty() && saved.warning.isEmpty());
        const auto recorded = testing::readBaseline(stick);
        assert(recorded && recorded->pdbSequence == testing::FixtureSequence);
        assert(recorded->tracks.size() == 1 && recorded->tracks[0].rating == 5);
        assert(testing::fileBytes(testing::baselineFileOf(stick)) != planted);

        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        reportError(undone, "undo");
        assert(undone.error.isEmpty());
        assert(testing::fileBytes(testing::baselineFileOf(stick)) == planted && "Undo puts the earlier baseline back");
        std::cout << "case 2 (over an earlier baseline; Undo restores it byte for byte) OK\n";
    }

    // 3. A save that stops: the landed change's item is recorded, the
    //    failed change's keeps the previous value. Then a save where the
    //    first change fails records nothing at all.
    {
        const auto stick = testing::makeEngineChangeStick(fixture, "record_stopped");
        testing::levelImportCounter(stick);
        const auto tracks = testing::rekordboxTracks(stick, 2);
        const std::string k1 = testing::keyOf(stick, tracks[0]);
        const std::string k2 = testing::keyOf(stick, tracks[1]);
        domain::RekordboxBaseline previous;
        previous.pdbSequence = 100;
        previous.tracks = {testing::baselineRow(stick, tracks[0], 1, ValueOrigin::Rekordbox, 4000.0),
                           testing::baselineRow(stick, tracks[1], 1, ValueOrigin::Rekordbox, 4000.0)};
        testing::plantBaseline(stick, previous);

        domain::RekordboxBaseline next;
        next.tracks = {testing::baselineRow(stick, tracks[0], 3, ValueOrigin::Unknown, 4000.0),
                       testing::baselineRow(stick, tracks[1], 3, ValueOrigin::Unknown, 4000.0)};
        const QString failId = QStringLiteral("rekordbox-export-sync:fail");
        const RecordRekordboxBaselineChange::AppliedBy appliedBy = {
            {SyncOntoEngineId.toStdString(), {"rating:" + k1}}, {failId.toStdString(), {"rating:" + k2}}};
        const SaveLoopResult saved = testing::saveChanges(
            {owned(testing::syncOntoEngine(stick)), std::make_shared<testing::FailingChange>(failId),
             record(stick, next, appliedBy)},
            stick.pioneerPath(), stick.enginePath());
        assert(saved.failedId == failId);
        assert((saved.appliedIds == QStringList{SyncOntoEngineId}) && "the record change itself was not reached");
        const auto recorded = testing::readBaseline(stick);
        assert(recorded && recorded->pdbSequence == testing::FixtureSequence);
        assert(recorded->tracks.size() == 2);
        assert(recorded->findTrack(k1)->rating == 3 && recorded->findTrack(k1)->ratingOrigin == ValueOrigin::Unknown
               && "landed: the new value");
        assert(recorded->findTrack(k2)->rating == 1 && recorded->findTrack(k2)->ratingOrigin == ValueOrigin::Rekordbox
               && "did not land: the previous value and origin");

        const std::string before = testing::fileBytes(testing::baselineFileOf(stick));
        const SaveLoopResult nothing = testing::saveChanges(
            {std::make_shared<testing::FailingChange>(failId), record(stick, next, appliedBy)}, stick.pioneerPath(),
            stick.enginePath());
        assert(nothing.failedId == failId && nothing.appliedIds.isEmpty());
        assert(testing::fileBytes(testing::baselineFileOf(stick)) == before && "nothing landed, nothing recorded");
        std::cout << "case 3 (a stopped save records only what landed; nothing landed records nothing) OK\n";
    }

    // 4. The ledger: a wrapped sync onto a rekordbox track writes its cues,
    //    and the record has them as the track's set, of Seabass origin,
    //    in place of what `next` said.
    {
        const auto stick = testing::makeEngineChangeStick(fixture, "record_ledger");
        testing::levelImportCounter(stick);
        const auto tracks = testing::rekordboxTracks(stick, 1);
        const std::string k1 = testing::keyOf(stick, tracks[0]);
        domain::RekordboxBaseline next;
        next.tracks = {testing::baselineRow(stick, tracks[0], std::nullopt, ValueOrigin::Unknown, 9000.0)};
        const auto sync = owned(testing::syncOntoRekordbox(stick, tracks[0], {CuePoint{CuePoint::Kind::Hot, 1, 1234.0, "", "synced"}}));
        const QString syncId = QStringLiteral("rekordbox-export-sync:sync:rekordbox:") + QString::fromStdString(tracks[0].sourceId);
        assert(sync->id() == syncId);
        const SaveLoopResult saved = testing::saveChanges(
            {sync, record(stick, next, {{syncId.toStdString(), {"cue:hot:1:" + k1, "cue:hot:2:" + k1}}})},
            stick.pioneerPath(), stick.enginePath());
        reportError(saved, "save");
        assert(saved.error.isEmpty() && saved.warning.isEmpty());
        assert(saved.appliedIds.size() == 2);
        const auto recorded = testing::readBaseline(stick);
        assert(recorded && recorded->pdbSequence == testing::FixtureSequence);
        const auto &row = *recorded->findTrack(k1);
        assert(row.cues.size() == 1);
        assert(row.cues[0].cue.kind == CuePoint::Kind::Hot && row.cues[0].cue.hotCueNumber == 1
               && row.cues[0].cue.positionMs == 1234.0 && row.cues[0].origin == ValueOrigin::Seabass);
        assert(!row.rating && row.ratingOrigin == ValueOrigin::Unknown && "no rating written, none recorded");
        std::cout << "case 4 (a wrapped sync onto rekordbox is in the ledger, Seabass origin) OK\n";
    }

    std::cout << "record_rekordbox_baseline_change_test: all passed\n";
    return 0;
}
