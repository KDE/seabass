// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The origin ledger in saves that are not Sync after Rekordbox Export's
// own: runSaveLoop merges what a save wrote onto the rekordbox side into
// the stick's baseline as Seabass's, and advances the baseline's sequence
// only when it was current at save start. On a copy of the anonymized
// fixture with both catalogs and the import counter levelled (export.pdb
// at 15132); the baseline planted by hand, two tracks each with one pad at
// 4000 ms of rekordbox origin, the first rated 1.
//
// 1. A Sync save onto a rekordbox track over a current baseline: the
//    track's cues are the written set, of Seabass origin; the sequence is
//    export.pdb's after the save (a cue write leaves it at 15132).
// 2. A Restore Metadata save (cues and a rating onto rekordbox) over a
//    current baseline: the rating write stamps export.pdb 15133, and the
//    baseline advances to it, with the cues and the rating of Seabass
//    origin. Undo puts the baseline back byte for byte.
// 3. The same save over a stale baseline (sequence 100: rekordbox
//    exported since): the ledger is merged and the sequence stays 100.
// 4. A save with only Engine writes leaves the baseline byte for byte.

#include <QCoreApplication>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

#include "domain/metadata_restore.hpp"
#include "gui/edit/changes/restore_metadata_change.hpp"
#include "rekordbox_baseline_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
using domain::CuePoint;
using domain::ValueOrigin;
namespace fs = std::filesystem;

namespace
{

struct Planted
{
    testing::EngineChangeStick stick;
    std::vector<domain::Track> tracks;
    std::string k1;
    std::string k2;
    std::string bytes;
};

Planted plantedStick(const fs::path &fixture, const std::string &name, std::uint64_t sequence)
{
    Planted p{testing::makeEngineChangeStick(fixture, name), {}, {}, {}, {}};
    testing::levelImportCounter(p.stick);
    p.tracks = testing::rekordboxTracks(p.stick, 2);
    p.k1 = testing::keyOf(p.stick, p.tracks[0]);
    p.k2 = testing::keyOf(p.stick, p.tracks[1]);
    domain::RekordboxBaseline baseline;
    baseline.pdbSequence = sequence;
    baseline.engineUuid = "uuid-ledger";
    baseline.tracks = {testing::baselineRow(p.stick, p.tracks[0], 1, ValueOrigin::Rekordbox, 4000.0),
                       testing::baselineRow(p.stick, p.tracks[1], std::nullopt, ValueOrigin::Rekordbox, 4000.0)};
    testing::plantBaseline(p.stick, baseline);
    p.bytes = testing::fileBytes(testing::baselineFileOf(p.stick));
    return p;
}

// Restore Metadata's change for the second track: one pad and 4 stars
// back onto rekordbox (its analysis file, export.pdb and the OneLibrary
// row).
std::shared_ptr<PendingChange> restoreOntoRekordbox(const Planted &p)
{
    domain::MetadataRestoreProposal proposal;
    proposal.stickTrack = p.tracks[1];
    proposal.cues = {CuePoint{CuePoint::Kind::Hot, 3, 3333.0, "", "restored"}};
    proposal.cuesOffered = true;
    proposal.rating = 4;
    proposal.ratingOffered = true;
    return std::make_shared<RestoreMetadataChange>(QStringLiteral("rekordbox"), p.stick.pioneerPath(),
                                                   QString::fromStdString(p.tracks[1].sourceId), proposal, 1);
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

// The second track as the planted baseline has it, untouched.
void secondTrackAsPlanted(const domain::RekordboxBaseline &b, const std::string &k2)
{
    const auto &row = *b.findTrack(k2);
    assert(!row.rating && row.ratingOrigin == ValueOrigin::Rekordbox);
    assert(row.cues.size() == 1 && row.cues[0].cue.positionMs == 4000.0 && row.cues[0].origin == ValueOrigin::Rekordbox);
}

// The second track after the restore: the restored pad and 4 stars, both
// Seabass's.
void secondTrackRestored(const domain::RekordboxBaseline &b, const std::string &k2)
{
    const auto &row = *b.findTrack(k2);
    assert(row.rating == 4 && row.ratingOrigin == ValueOrigin::Seabass);
    assert(row.cues.size() == 1 && row.cues[0].cue.hotCueNumber == 3 && row.cues[0].cue.positionMs == 3333.0
           && row.cues[0].origin == ValueOrigin::Seabass);
}

// The first track as planted: rated 1, one pad at 4000, rekordbox's.
void firstTrackAsPlanted(const domain::RekordboxBaseline &b, const std::string &k1)
{
    const auto &row = *b.findTrack(k1);
    assert(row.rating == 1 && row.ratingOrigin == ValueOrigin::Rekordbox);
    assert(row.cues.size() == 1 && row.cues[0].cue.positionMs == 4000.0 && row.cues[0].origin == ValueOrigin::Rekordbox);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: rekordbox_baseline_ledger_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    const fs::path scratch = testing::scratchRoot() / "rekordbox_baseline_ledger_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    testing::sandboxSeabassHome(scratch / "home");
    testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    // 1. Sync onto rekordbox over a current baseline.
    {
        const Planted p = plantedStick(fixture, "ledger_sync", testing::FixtureSequence);
        const SaveLoopResult saved = testing::saveChanges(
            {std::shared_ptr<PendingChange>(testing::syncOntoRekordbox(
                p.stick, p.tracks[0],
                {CuePoint{CuePoint::Kind::Hot, 1, 1234.0, "", ""}, CuePoint{CuePoint::Kind::Memory, 0, 800.0, "", ""}}))},
            p.stick.pioneerPath(), p.stick.enginePath());
        reportError(saved, "save");
        assert(saved.error.isEmpty() && saved.warning.isEmpty() && saved.appliedIds.size() == 1);
        assert(testing::pdbSequence(p.stick) == testing::FixtureSequence && "a cue write leaves export.pdb alone");
        const auto b = testing::readBaseline(p.stick);
        assert(b && b->pdbSequence == testing::FixtureSequence && b->engineUuid == "uuid-ledger");
        const auto &row = *b->findTrack(p.k1);
        assert(row.cues.size() == 2);
        assert(row.cues[0].cue.hotCueNumber == 1 && row.cues[0].cue.positionMs == 1234.0
               && row.cues[0].origin == ValueOrigin::Seabass);
        assert(row.cues[1].cue.kind == CuePoint::Kind::Memory && row.cues[1].cue.positionMs == 800.0
               && row.cues[1].origin == ValueOrigin::Seabass);
        assert(row.rating == 1 && row.ratingOrigin == ValueOrigin::Rekordbox && "no rating written, the old one stays");
        secondTrackAsPlanted(*b, p.k2);
        std::cout << "case 1 (Sync onto rekordbox: written cues of Seabass origin, sequence 15132) OK\n";
    }

    // 2. Restore Metadata onto rekordbox over a current baseline: the
    //    sequence advances to export.pdb's after the save; Undo puts the
    //    baseline back.
    {
        const Planted p = plantedStick(fixture, "ledger_current", testing::FixtureSequence);
        const std::string pdbBefore = testing::fileBytes(p.stick.pioneer / "rekordbox" / "export.pdb");
        const SaveLoopResult saved =
            testing::saveChanges({restoreOntoRekordbox(p)}, p.stick.pioneerPath(), p.stick.enginePath());
        reportError(saved, "save");
        assert(saved.error.isEmpty() && saved.warning.isEmpty() && saved.appliedIds.size() == 1);
        assert(testing::pdbSequence(p.stick) == 15133 && "the rating write stamps export.pdb one on");
        const auto b = testing::readBaseline(p.stick);
        assert(b && b->pdbSequence == 15133 && "current at save start: advanced");
        secondTrackRestored(*b, p.k2);
        firstTrackAsPlanted(*b, p.k1);

        const SaveLoopResult undone = testing::undoSave(saved, p.stick.pioneerPath(), p.stick.enginePath());
        reportError(undone, "undo");
        assert(undone.error.isEmpty() && undone.appliedIds.size() == 1);
        assert(testing::fileBytes(testing::baselineFileOf(p.stick)) == p.bytes && "Undo restores the baseline");
        assert(testing::fileBytes(p.stick.pioneer / "rekordbox" / "export.pdb") == pdbBefore);
        std::cout << "case 2 (current baseline advances to 15133; Undo restores it byte for byte) OK\n";
    }

    // 3. The same save over a stale baseline: merged, not advanced.
    {
        const Planted p = plantedStick(fixture, "ledger_stale", 100);
        const SaveLoopResult saved =
            testing::saveChanges({restoreOntoRekordbox(p)}, p.stick.pioneerPath(), p.stick.enginePath());
        reportError(saved, "save");
        assert(saved.error.isEmpty() && saved.warning.isEmpty());
        assert(testing::pdbSequence(p.stick) == 15133);
        const auto b = testing::readBaseline(p.stick);
        assert(b && b->pdbSequence == 100 && "not current at save start: the sequence stays");
        secondTrackRestored(*b, p.k2);
        firstTrackAsPlanted(*b, p.k1);
        std::cout << "case 3 (stale baseline: ledger merged, sequence left at 100) OK\n";
    }

    // 4. Engine writes only: the baseline is not touched.
    {
        const Planted p = plantedStick(fixture, "ledger_engine_only", testing::FixtureSequence);
        const SaveLoopResult saved = testing::saveChanges(
            {std::shared_ptr<PendingChange>(testing::syncOntoEngine(p.stick))}, p.stick.pioneerPath(),
            p.stick.enginePath());
        reportError(saved, "save");
        assert(saved.error.isEmpty() && saved.appliedIds.size() == 1);
        assert(testing::fileBytes(testing::baselineFileOf(p.stick)) == p.bytes);
        std::cout << "case 4 (Engine-only save leaves the baseline byte for byte) OK\n";
    }

    std::cout << "rekordbox_baseline_ledger_test: all passed\n";
    return 0;
}
