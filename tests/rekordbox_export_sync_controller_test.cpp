// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Sync after Rekordbox Export through RekordboxExportSyncController, on
// copies of tests/fixtures/anonymized_library (never the fixture itself).
//
// The fixture is a stick in "Sync Needed" state with no baseline: export.pdb
// at 15132, Engine's import counter at 14204. The proposal's numbers were
// pinned by hand by the planner's own steps (seabass-cli sync-after-export
// on the same fixture, steps 2b and 13): 1 playlist to create, 925
// membership rows, 1245 conflicts, 1177 of Engine's own kept, 953 rows
// ticked. Counted here off the model the page binds to, so a model that
// dropped or folded a row fails against the fixture rather than itself.
//
// 1. analyze fills the model with those counts and says it had no record.
// 2. dependsOn both ways: unticking the create unticks its members, ticking
//    a member ticks the create; whole sections.
// 3. An import conflict answered toward rekordbox becomes a removal row
//    under the conflict's key; answered toward Engine, none.
// 4. A small selection (the create, its members, one cue row) staged and
//    saved through the session: Engine has the playlist with those
//    members, the record is on the stick at 15132 with the applied items
//    at rekordbox's value and the declined ones out of it and listed as
//    declined, the analysis after the save no longer lists any of them,
//    and the import counter is level. Undo puts m.db back (but for the
//    import counter, which every save with both catalogs levels) and
//    takes the record away.
// 5. A damaged record fails the analysis with its reason and an empty
//    list: refused, never a fallback.
//
// argv[1]: tests/fixtures/anonymized_library.

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "engine_change_fixture.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/rekordbox_export_sync_controller.hpp"
#include "gui/rekordbox_export_sync_list_model.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "rekordbox_baseline_fixture.hpp"
#include "scratch_path.hpp"

using seabass::pathFromUtf8;
using seabass::pathToUtf8;
using seabass::gui::EditSessionRegistry;
using seabass::gui::LibraryEditSession;
using seabass::gui::RekordboxExportSyncController;
using seabass::gui::RekordboxExportSyncListModel;
using Section = RekordboxExportSyncListModel::Section;
using Row = RekordboxExportSyncListModel::Row;
namespace fs = std::filesystem;
namespace testing = seabass::testing;

namespace
{

constexpr int FixtureCreates = 1;
constexpr int FixtureMembership = 925;
constexpr int FixtureConflicts = 1245;
constexpr int FixtureKept = 1177;
constexpr int FixtureChecked = 953;

void waitUntilIdle(RekordboxExportSyncController &controller)
{
    QSignalSpy busy(&controller, &RekordboxExportSyncController::busyChanged);
    for (int i = 0; i < 3000 && controller.busy(); ++i) {
        busy.wait(100);
    }
    assert(!controller.busy());
}

void analyze(RekordboxExportSyncController &controller, const testing::EngineChangeStick &stick)
{
    controller.analyze(QStringLiteral("TEST"), stick.pioneerPath(), stick.enginePath());
    waitUntilIdle(controller);
}

int count(const RekordboxExportSyncController &controller, const char *section)
{
    return controller.sectionCounts().value(QString::fromLatin1(section)).toInt();
}

void printCounts(const RekordboxExportSyncController &controller)
{
    const QVariantMap counts = controller.sectionCounts();
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        std::cout << "  " << it.key().toStdString() << ": " << it.value().toInt() << "\n";
    }
    std::cout << "  checked: " << controller.checkedCount() << ", conflicts open: " << controller.conflictCount()
              << "\n";
}

const std::vector<Row> &rowsOf(RekordboxExportSyncController &controller)
{
    return controller.rows()->rows();
}

int firstRow(RekordboxExportSyncController &controller, Section section, const QString &kind = QString())
{
    const auto &rows = rowsOf(controller);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].section == section && (kind.isEmpty() || rows[i].kind == kind)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool dependsOn(const Row &row, const std::string &key)
{
    return std::find(row.header.dependsOn.begin(), row.header.dependsOn.end(), key) != row.header.dependsOn.end();
}

std::vector<int> rowsDependingOn(RekordboxExportSyncController &controller, const std::string &key)
{
    std::vector<int> out;
    const auto &rows = rowsOf(controller);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (RekordboxExportSyncListModel::writable(rows[i].section) && dependsOn(rows[i], key)) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

bool included(RekordboxExportSyncController &controller, int row)
{
    return rowsOf(controller)[static_cast<std::size_t>(row)].included;
}

std::set<std::string> keysListed(RekordboxExportSyncController &controller)
{
    std::set<std::string> keys;
    for (const auto &row : rowsOf(controller)) {
        keys.insert(row.header.key);
        for (const auto &item : row.header.cueItems) {
            keys.insert(item.key);
        }
    }
    return keys;
}

LibraryEditSession *sessionOf(const testing::EngineChangeStick &stick)
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(stick.pioneerPath()));
    assert(session);
    return session;
}

QVariantMap saveAndWait(LibraryEditSession *session, RekordboxExportSyncController &controller,
                        void (*start)(LibraryEditSession *, RekordboxExportSyncController &))
{
    QSignalSpy finished(session, &LibraryEditSession::saveFinished);
    start(session, controller);
    for (int i = 0; i < 1200 && finished.isEmpty(); ++i) {
        finished.wait(100);
    }
    assert(!finished.isEmpty() && "the save finished");
    // The page is analysed again after every save of its session.
    waitUntilIdle(controller);
    return finished.takeFirst().at(0).toMap();
}

// 1, 2, 3 on a copy no save touches.
void testProposalTicksAndAnswers(const fs::path &fixture)
{
    const auto stick = testing::makeEngineChangeStick(fixture, "export_sync_proposal");
    RekordboxExportSyncController controller;
    analyze(controller, stick);
    if (!controller.errorMessage().isEmpty()) {
        std::cerr << controller.errorMessage().toStdString() << "\n";
    }
    assert(controller.errorMessage().isEmpty());
    assert(controller.analyzed());
    printCounts(controller);

    // 1. The fixture's proposal, and what it was compared against.
    assert(!controller.hasBaseline());
    assert(controller.currentSequence() == 15132);
    assert(controller.introText()
           == QStringLiteral("No earlier record of this stick: additions are assumed, removals are left to you"));
    assert(count(controller, "playlists") == FixtureCreates);
    assert(firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist")) >= 0);
    assert(count(controller, "membership") == FixtureMembership);
    assert(count(controller, "conflicts") == FixtureConflicts);
    assert(controller.conflictCount() == FixtureConflicts);
    assert(count(controller, "engineOwnKept") == FixtureKept);
    assert(controller.checkedCount() == FixtureChecked);
    assert(controller.rows()->rowCount() == [&] {
        int total = 0;
        for (const auto &value : controller.sectionCounts()) {
            total += value.toInt();
        }
        return total;
    }());
    // Sections are contiguous and in the page's order, for section.property.
    {
        int last = -1;
        for (const auto &row : rowsOf(controller)) {
            assert(static_cast<int>(row.section) >= last);
            last = static_cast<int>(row.section);
        }
    }
    std::cout << "case 1 (the fixture's proposal: 1 create, 925 members, 1245 conflicts, 1177 kept, 953 ticked) OK\n";

    // 2. dependsOn, both ways.
    const int create = firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist"));
    const std::string createKey = rowsOf(controller)[static_cast<std::size_t>(create)].header.key;
    const std::vector<int> members = rowsDependingOn(controller, createKey);
    assert(!members.empty() && "the new playlist's members depend on its create");
    for (int m : members) {
        assert(included(controller, m));
    }
    controller.setIncluded(create, false);
    assert(!included(controller, create));
    for (int m : members) {
        assert(!included(controller, m) && "unticking the create unticks its members");
    }
    const int afterUntick = controller.checkedCount();
    assert(afterUntick <= FixtureChecked - 1 - static_cast<int>(members.size()));
    controller.setIncluded(members.front(), true);
    assert(included(controller, members.front()));
    assert(included(controller, create) && "ticking a member ticks its playlist's create");
    for (std::size_t i = 1; i < members.size(); ++i) {
        assert(!included(controller, members[i]) && "the other members stay as they were");
    }
    controller.setSectionIncluded(QStringLiteral("membership"), true);
    for (int m : members) {
        assert(included(controller, m));
    }
    assert(controller.checkedCount() == FixtureChecked);
    assert(controller.sectionCheckedCounts().value(QStringLiteral("membership")).toInt() == FixtureMembership);
    controller.setSectionIncluded(QStringLiteral("membership"), false);
    assert(controller.checkedCount() == FixtureChecked - FixtureMembership);
    assert(controller.sectionCheckedCounts().value(QStringLiteral("membership")).toInt() == 0);
    assert(controller.sectionCheckedCounts().value(QStringLiteral("playlists")).toInt() == FixtureCreates);
    assert(included(controller, create) && "unticking members leaves their playlist");
    controller.setSectionIncluded(QStringLiteral("playlists"), false);
    controller.setSectionIncluded(QStringLiteral("membership"), true);
    assert(included(controller, create) && "a section ticked brings what it depends on");
    assert(controller.checkedCount() == FixtureChecked);
    std::cout << "case 2 (dependsOn both ways; whole sections) OK\n";

    // 3. An import conflict answered: toward rekordbox it is a removal
    //    from Engine under the conflict's key, toward Engine nothing.
    int conflict = -1;
    for (std::size_t i = 0; i < rowsOf(controller).size(); ++i) {
        const Row &row = rowsOf(controller)[i];
        if (row.section == Section::Conflicts
            && row.header.reason == seabass::domain::EngineUpdateReason::NoBaselineImportedRow) {
            conflict = static_cast<int>(i);
            break;
        }
    }
    assert(conflict >= 0);
    const std::string conflictKey = rowsOf(controller)[static_cast<std::size_t>(conflict)].header.key;
    const int removalsBefore = count(controller, "tracksToRemove");
    const int checkedBefore = controller.checkedCount();
    controller.resolveConflict(conflict, true);
    assert(count(controller, "tracksToRemove") == removalsBefore + 1);
    assert(controller.conflictCount() == FixtureConflicts - 1);
    assert(controller.checkedCount() == checkedBefore + 1);
    int removal = -1;
    for (std::size_t i = 0; i < rowsOf(controller).size(); ++i) {
        const Row &row = rowsOf(controller)[i];
        if (row.section == Section::TracksToRemove && row.fromConflictUid >= 0) {
            removal = static_cast<int>(i);
        }
    }
    assert(removal >= 0);
    {
        const QModelIndex at = controller.rows()->index(removal);
        assert(at.data(RekordboxExportSyncListModel::KeyRole).toString().toStdString() == conflictKey);
        assert(at.data(RekordboxExportSyncListModel::IncludedRole).toBool());
        assert(at.data(RekordboxExportSyncListModel::ResolvedSideRole).toString() == QStringLiteral("rekordbox"));
        assert(at.data(RekordboxExportSyncListModel::DirectionRole).toString() == QStringLiteral("to Engine"));
        assert(at.data(RekordboxExportSyncListModel::KindRole).toString() == QStringLiteral("removeTrack"));
    }
    // The conflict row moved by nothing: answers go into their own section,
    // which comes before the conflicts.
    conflict += 1;
    assert(rowsOf(controller)[static_cast<std::size_t>(conflict)].header.key == conflictKey);
    controller.resolveConflict(conflict, false);
    assert(count(controller, "tracksToRemove") == removalsBefore);
    assert(controller.conflictCount() == FixtureConflicts - 1);
    conflict -= 1;
    assert(controller.rows()->index(conflict).data(RekordboxExportSyncListModel::ResolvedSideRole).toString()
           == QStringLiteral("engine"));
    controller.clearConflictResolution(conflict);
    assert(controller.conflictCount() == FixtureConflicts);
    assert(controller.checkedCount() == checkedBefore);
    std::cout << "case 3 (an import conflict answered: a removal toward rekordbox, nothing toward Engine) OK\n";

    // 5. A damaged record on this stick: the analysis fails with its
    //    reason, and the list it had is gone.
    const fs::path record = testing::baselineFileOf(stick);
    fs::create_directories(record.parent_path());
    {
        std::ofstream out(record, std::ios::binary);
        out << "this is not a rekordbox baseline";
    }
    controller.analyze(QStringLiteral("TEST"), stick.pioneerPath(), stick.enginePath());
    waitUntilIdle(controller);
    std::cout << "  damaged record: " << controller.errorMessage().toStdString() << "\n";
    assert(!controller.errorMessage().isEmpty());
    assert(controller.rows()->rowCount() == 0);
    assert(!controller.analyzed());
    assert(controller.checkedCount() == 0 && controller.introText().isEmpty());
    std::cout << "case 5 (a damaged record is refused: the error, an empty list) OK\n";
}

// 4. A small selection saved through the session, the record, the
//    analysis after it, and Undo.
void testSmallSave(const fs::path &fixture)
{
    const auto stick = testing::makeEngineChangeStick(fixture, "export_sync_save");
    const testing::DatabaseDump before = testing::dumpDatabase(stick.db);
    const auto informationBefore = testing::sqlRows(stick.db, "SELECT id, uuid, schemaVersionMajor, schemaVersionMinor, "
                                                              "schemaVersionPatch, currentPlayedIndiciator, "
                                                              "lastRekordBoxLibraryImportReadCounter FROM Information;");
    const fs::path record = testing::baselineFileOf(stick);
    assert(!fs::exists(record));

    RekordboxExportSyncController controller;
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());

    for (const char *section : {"playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                                "cuesToEngine", "restoresToRekordbox"}) {
        controller.setSectionIncluded(QString::fromLatin1(section), false);
    }
    assert(controller.checkedCount() == 0);

    const int create = firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist"));
    const Row createRow = rowsOf(controller)[static_cast<std::size_t>(create)];
    const std::string playlistPath = std::get<seabass::domain::PlaylistCreate>(*createRow.edit).path;
    std::vector<std::string> memberFiles;
    std::vector<std::string> memberKeys;
    std::vector<std::string> memberItems;
    for (int m : rowsDependingOn(controller, createRow.header.key)) {
        controller.setIncluded(m, true);
        memberItems.push_back(rowsOf(controller)[static_cast<std::size_t>(m)].header.key);
        const auto &edit = std::get<seabass::domain::MembershipEdit>(*rowsOf(controller)[static_cast<std::size_t>(m)].edit);
        assert(edit.kind == seabass::domain::MembershipEdit::Kind::Add);
        memberFiles.push_back(edit.track.filePath);
        memberKeys.push_back(edit.pathKey);
    }
    assert(included(controller, create));
    const int cue = firstRow(controller, Section::CuesToEngine);
    assert(cue >= 0 && "the fixture proposes cues for Engine");
    controller.setIncluded(cue, true);
    const Row cueRow = rowsOf(controller)[static_cast<std::size_t>(cue)];
    const int ticked = controller.checkedCount();
    assert(ticked == 1 + static_cast<int>(memberFiles.size()) + 1);

    // One declined membership of another playlist, to find in the record.
    std::string declinedKey;
    std::string declinedState;
    for (const auto &row : rowsOf(controller)) {
        if (row.section == Section::Membership && !row.included) {
            declinedKey = row.header.key;
            declinedState = row.header.rekordboxState;
            break;
        }
    }
    assert(!declinedKey.empty());

    controller.stageSelected();
    if (!controller.errorMessage().isEmpty()) {
        std::cerr << controller.errorMessage().toStdString() << "\n";
    }
    assert(controller.errorMessage().isEmpty());
    assert(controller.stagedCount() == ticked);
    LibraryEditSession *session = sessionOf(stick);
    // The create, one membership change for the playlist, the cues, and
    // the record last.
    std::cout << "  staged:\n";
    for (const auto &line : session->pendingDescriptions()) {
        std::cout << "    " << line.toStdString() << "\n";
    }
    assert(session->pendingCount() == 4);
    assert(session->editorOwner() == QStringLiteral("rekordbox-export-sync"));

    const QVariantMap summary = saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) {
        s->save();
    });
    std::cout << "  summary: " << summary.value("written").toInt() << " of " << summary.value("total").toInt() << " "
              << summary.value("unit").toString().toStdString() << " " << summary.value("verb").toString().toStdString()
              << "; error \"" << summary.value("error").toString().toStdString() << "\", warning \""
              << summary.value("warning").toString().toStdString() << "\"\n";
    assert(summary.value("error").toString().isEmpty());
    assert(summary.value("warning").toString().isEmpty());
    // Three kinds of change in one save are told in changes.
    assert(summary.value("unit").toString() == QStringLiteral("changes"));
    assert(summary.value("verb").toString() == QStringLiteral("made"));
    assert(summary.value("written").toInt() == 3 && summary.value("total").toInt() == 3);
    assert(controller.stagedCount() == 0);

    // Engine has the playlist, its members in rekordbox's order.
    {
        const std::string leaf = playlistPath.substr(playlistPath.rfind('/') == std::string::npos
                                                         ? 0
                                                         : playlistPath.rfind('/') + 1);
        assert(playlistPath.find('/') == std::string::npos && "the fixture's new playlist is at the top level");
        const auto tracks = testing::rootPlaylistTracks(stick.db, leaf);
        assert(tracks.size() == memberFiles.size());
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const auto path = testing::sqlRows(stick.db, "SELECT path FROM Track WHERE id = " + std::to_string(tracks[i]) + ";");
            assert(path.size() == 1);
            const std::string file = pathToUtf8(pathFromUtf8(memberFiles[i]).filename());
            assert(path[0][0].size() >= file.size()
                   && path[0][0].compare(path[0][0].size() - file.size(), file.size(), file) == 0);
        }
    }

    // The record: at 15132, the applied items at rekordbox's value, the
    // declined membership out of its playlist and listed as declined.
    assert(fs::exists(record));
    const auto baseline = testing::readBaseline(stick);
    assert(baseline);
    assert(baseline->pdbSequence == 15132);
    const auto *created = baseline->findPlaylistByPath(playlistPath);
    assert(created && created->members == memberKeys);
    {
        const auto &edit = std::get<seabass::domain::CueEdit>(*cueRow.edit);
        const auto *track = baseline->findTrack(edit.pathKey);
        assert(track && "the track whose cues went to Engine is recorded");
    }
    assert(baseline->declined.count(declinedKey) == 1 && baseline->declined.at(declinedKey) == declinedState);
    {
        const auto parsed = seabass::domain::parseItemKey(declinedKey);
        assert(parsed && parsed->kind == seabass::domain::ItemKey::Kind::Member);
        const auto *playlist = baseline->findPlaylist(parsed->playlistId);
        assert(playlist);
        assert(std::find(playlist->members.begin(), playlist->members.end(), parsed->pathKey) == playlist->members.end()
               && "a declined member is not recorded as there");
    }

    // The analysis after the save: none of the applied rows, none of the
    // declined ones, and the import counter level.
    printCounts(controller);
    assert(controller.hasBaseline());
    assert(controller.introText()
           == QStringLiteral("Compared with how this stick looked when Seabass last saved it, export 15132"));
    const auto listed = keysListed(controller);
    assert(!listed.count(createRow.header.key));
    for (const auto &key : memberItems) {
        assert(!listed.count(key));
    }
    for (const auto &item : cueRow.header.cueItems) {
        assert(!listed.count(item.key));
    }
    assert(!listed.count(cueRow.header.key));
    assert(!listed.count(declinedKey));
    for (const auto &row : rowsOf(controller)) {
        assert(!RekordboxExportSyncListModel::writable(row.section) && "only conflicts and Engine's own are left");
    }
    const auto importState = seabass::infrastructure::engine::readRekordboxImportState(stick.engineUtf8(),
                                                                                       pathToUtf8(stick.pioneer));
    assert(importState.error.empty() && !importState.playerWillOfferImport());
    std::cout << "case 4a (a small save: the playlist and its members on Engine, the record at 15132, the "
                 "analysis after it empty but for conflicts and Engine's own) OK\n";

    // Undo: m.db as it was, and no record.
    assert(controller.canUndo());
    const QVariantMap undone = saveAndWait(session, controller, [](LibraryEditSession *, RekordboxExportSyncController &c) {
        c.undoLastOperation();
    });
    assert(undone.value("error").toString().isEmpty());
    // Every table as it was but Information's import counter: Undo is a
    // save with both catalogs too, and every such save leaves the player's
    // import record level with export.pdb (keepImportLevel in
    // save_loop.cpp), so the 14204 the fixture had comes back as 15132.
    testing::DatabaseDump after = testing::dumpDatabase(stick.db);
    testing::DatabaseDump beforeButInformation = before;
    beforeButInformation.erase("Information");
    after.erase("Information");
    const auto diffs = testing::differences(beforeButInformation, after);
    testing::printDifferences(diffs);
    assert(diffs.empty());
    const auto information = testing::sqlRows(stick.db, "SELECT id, uuid, schemaVersionMajor, schemaVersionMinor, "
                                                        "schemaVersionPatch, currentPlayedIndiciator, "
                                                        "lastRekordBoxLibraryImportReadCounter FROM Information;");
    assert(information.size() == informationBefore.size() && information.size() == 1);
    for (std::size_t c = 0; c + 1 < information[0].size(); ++c) {
        assert(information[0][c] == informationBefore[0][c]);
    }
    assert(informationBefore[0].back() == "14204" && information[0].back() == "15132");
    assert(!fs::exists(record));
    assert(!controller.hasBaseline() && count(controller, "membership") == FixtureMembership);
    std::cout << "case 4b (Undo puts m.db back and takes the record away) OK\n";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: rekordbox_export_sync_controller_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    const fs::path scratch = testing::scratchRoot() / "rekordbox_export_sync_controller_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    testing::sandboxSeabassHome(scratch / "home");
    testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    testProposalTicksAndAnswers(fixture);
    testSmallSave(fixture);
    std::cout << "rekordbox_export_sync_controller_test: all cases OK\n";
    return 0;
}
