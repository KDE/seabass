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
// membership rows, 1245 conflicts, 1160 of Engine's own kept, 17 playlists only Engine has, 953 rows
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
// 6. Every conflict answered at once: toward rekordbox every import
//    conflict has its ticked removal row, cleared none has.
// 7. The section order (conflicts first when there are any, playlists
//    first when there are none) and the details of an add, a membership
//    and a cue row, pinned by hand on a proposal built here.
// 8. An order question left unanswered on a run without a record is not
//    asked again (the plan's step 10 review decision): a playlist saved
//    onto Engine, its record taken away and two of its Engine members
//    swapped is an order conflict; a save that leaves it unanswered
//    records rekordbox's order with every member, so the next analysis
//    keeps Engine's order as its own and asks nothing about it.
//
// argv[1]: tests/fixtures/anonymized_library.

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <utility>
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
// Engine's own, without the playlists only Engine has, which have their
// own section.
constexpr int FixtureKept = 1160;
constexpr int FixtureEngineOnlyPlaylists = 17;
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
           == QStringLiteral("No earlier record of this stick. What rekordbox has and Engine lacks is taken as added in "
                             "rekordbox and is selected; what Engine has and rekordbox lacks is left for you to "
                             "decide. Anything you leave undecided counts as Engine's own from now on and is not "
                             "asked again."));
    assert(count(controller, "playlists") == FixtureCreates);
    assert(firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist")) >= 0);
    assert(count(controller, "membership") == FixtureMembership);
    assert(count(controller, "conflicts") == FixtureConflicts);
    assert(controller.conflictCount() == FixtureConflicts);
    std::cout << "  kept " << count(controller, "engineOwnKept") << ", Engine-only playlists "
              << count(controller, "engineOnlyPlaylists") << "\n";
    assert(count(controller, "engineOwnKept") == FixtureKept);
    assert(count(controller, "engineOnlyPlaylists") == FixtureEngineOnlyPlaylists);
    for (const auto &row : rowsOf(controller)) {
        if (row.section == Section::EngineOnlyPlaylists) {
            assert(!row.included && row.title.size() > 0);
            assert(row.detail == QStringLiteral("folder") || row.detail.endsWith(QStringLiteral(" track"))
                   || row.detail.endsWith(QStringLiteral(" tracks")));
        } else if (row.section == Section::EngineOwnKept) {
            // Its playlists are ones rekordbox has too, or members.
            assert(!(row.header.key.empty() && row.kind == QStringLiteral("kept") && !row.hasTrack
                     && row.header.reason == seabass::domain::EngineUpdateReason::NoBaselineEngineOnly));
        }
    }
    assert(controller.checkedCount() == FixtureChecked);
    assert(controller.rows()->rowCount() == [&] {
        int total = 0;
        for (const auto &value : controller.sectionCounts()) {
            total += value.toInt();
        }
        return total;
    }());
    // Conflicts come first: they are the only rows that need an answer.
    assert(rowsOf(controller).front().section == Section::Conflicts);
    assert(controller.rows()->index(0).data(RekordboxExportSyncListModel::SectionRole).toString()
           == QStringLiteral("conflicts"));
    // Sections are contiguous and in the page's order, for section.property.
    {
        int last = -1;
        for (const auto &row : rowsOf(controller)) {
            assert(static_cast<int>(row.section) >= last);
            last = static_cast<int>(row.section);
        }
    }
    // The overview bar's categories: membership split into its adds
    // (changed) and its removes (removed), counted here off the rows.
    {
        int adds = 0;
        int removes = 0;
        int changedElsewhere = 0;
        for (const auto &row : rowsOf(controller)) {
            if (row.section == Section::Membership) {
                (row.kind == QStringLiteral("addMember") ? adds : removes) += 1;
            } else if (row.section == Section::MetadataToEngine || row.section == Section::CuesToEngine
                       || row.section == Section::RestoresToRekordbox) {
                ++changedElsewhere;
            }
        }
        const QVariantMap categories = controller.categoryCounts();
        std::cout << "  categories: new tracks " << categories.value("newTracks").toInt() << ", new playlists "
                  << categories.value("newPlaylists").toInt() << ", changed " << categories.value("changed").toInt()
                  << ", removed " << categories.value("removed").toInt() << ", other "
                  << categories.value("other").toInt() << " (membership " << adds << " in, " << removes << " out)\n";
        assert(adds + removes == FixtureMembership);
        assert(categories.value("newTracks").toInt() == count(controller, "tracksToAdd"));
        assert(categories.value("newPlaylists").toInt() == FixtureCreates);
        assert(categories.value("changed").toInt() == adds + changedElsewhere);
        assert(categories.value("removed").toInt() == removes + count(controller, "tracksToRemove"));
        assert(categories.value("other").toInt() == 0);
        // 925 membership adds, 1 rating or comment, 17 cue rows, 9 restores.
        assert(categories.value("changed").toInt() == 952);
        assert(!controller.proposalEmpty());
    }
    // The one line over the page, counted here off the rows: the
    // membership rows' distinct tracks, a track in two playlists once.
    {
        std::set<std::string> moved;
        int added = 0;
        int removed = 0;
        for (const auto &row : rowsOf(controller)) {
            if (row.section == Section::Membership) {
                moved.insert(std::get<seabass::domain::MembershipEdit>(*row.edit).pathKey);
            } else if (row.section == Section::TracksToAdd) {
                ++added;
            } else if (row.section == Section::TracksToRemove) {
                ++removed;
            }
        }
        const QVariantMap legend = controller.legendTexts();
        std::cout << "  legend: " << legend.value("newPlaylists").toString().toStdString() << " | "
                  << legend.value("changed").toString().toStdString() << " | "
                  << legend.value("removed").toString().toStdString() << " | " << moved.size() << "\n";
        const QVariantMap summary = controller.summaryCounts();
        assert(summary.value("tracksAdded").toInt() == added);
        assert(summary.value("tracksRemoved").toInt() == removed);
        assert(summary.value("tracksMoved").toInt() == static_cast<int>(moved.size()));
        assert(summary.value("playlistsCreated").toInt() == FixtureCreates);
        assert(legend.value("newPlaylists").toString() == QStringLiteral("1 playlist created"));
        assert(legend.value("changed").toString() == QStringLiteral("779 tracks moved between playlists, 1 rating or comment changed, 17 tracks' cues changed, 9 restores onto rekordbox"));
    }

    // The conflicts' buttons say what they do, as the page shows them
    // (the model's label roles), for the three kinds the fixture holds;
    // a row's details are headed by the same words.
    {
        std::set<std::pair<QString, QString>> pairs;
        int imported = -1;
        for (int i = 0; i < controller.rows()->rowCount(); ++i) {
            const QModelIndex at = controller.rows()->index(i);
            if (!at.data(RekordboxExportSyncListModel::IsConflictRole).toBool()) {
                continue;
            }
            pairs.emplace(at.data(RekordboxExportSyncListModel::RekordboxChoiceLabelRole).toString(),
                          at.data(RekordboxExportSyncListModel::EngineChoiceLabelRole).toString());
            if (imported < 0 && rowsOf(controller)[static_cast<std::size_t>(i)].header.reason
                    == seabass::domain::EngineUpdateReason::NoBaselineImportedRow) {
                imported = i;
            }
        }
        // An imported row rekordbox no longer lists.
        assert(pairs.count({QStringLiteral("Remove this track from Engine"), QStringLiteral("Keep it in Engine")}));
        // A member only Engine's playlist holds.
        assert(pairs.count({QStringLiteral("Take it out of \"Playlist 007\" in Engine"),
                            QStringLiteral("Keep it in \"Playlist 007\"")}));
        // Three rekordbox playlists spelled alike at one path.
        assert(pairs.count({QStringLiteral("Create one Engine playlist \"Playlist\" holding all 3 of them"),
                            QStringLiteral("Leave Engine without \"Playlist\"")}));
        assert(imported >= 0);
        const QStringList lines = controller.rows()->details(imported);
        assert(lines.contains(QStringLiteral("Remove this track from Engine (rekordbox's side):")));
        assert(lines.contains(QStringLiteral("Keep it in Engine (Engine's side):")));
    }
    std::cout << "case 1 (the fixture's proposal: 1 create, 925 members, 1245 conflicts, 1160 kept, 17 Engine-only playlists, 953 ticked) OK\n";

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
    // The conflict row did not move: answers go into their own sections,
    // which come after the conflicts.
    assert(rowsOf(controller)[static_cast<std::size_t>(conflict)].header.key == conflictKey);
    controller.resolveConflict(conflict, false);
    assert(count(controller, "tracksToRemove") == removalsBefore);
    assert(controller.conflictCount() == FixtureConflicts - 1);
    assert(controller.rows()->index(conflict).data(RekordboxExportSyncListModel::ResolvedSideRole).toString()
           == QStringLiteral("engine"));
    controller.clearConflictResolution(conflict);
    assert(controller.conflictCount() == FixtureConflicts);
    assert(controller.checkedCount() == checkedBefore);
    std::cout << "case 3 (an import conflict answered: a removal toward rekordbox, nothing toward Engine) OK\n";

    // 6. Every conflict at once, the one answered above included.
    controller.resolveConflict(conflict, false);
    std::set<std::string> importConflicts;
    for (const auto &row : rowsOf(controller)) {
        if (row.section == Section::Conflicts
            && row.header.reason == seabass::domain::EngineUpdateReason::NoBaselineImportedRow) {
            importConflicts.insert(row.header.key);
        }
    }
    assert(!importConflicts.empty());
    const auto tickedRemovalKeys = [&controller] {
        std::set<std::string> keys;
        for (const auto &row : rowsOf(controller)) {
            if (row.section == Section::TracksToRemove && row.fromConflictUid >= 0 && row.included) {
                keys.insert(row.header.key);
            }
        }
        return keys;
    };
    {
        QSignalSpy listChanged(&controller, &RekordboxExportSyncController::listChanged);
        controller.resolveAllConflicts(true);
        assert(!listChanged.isEmpty());
    }
    const std::set<std::string> removalsAnswered = tickedRemovalKeys();
    for (const auto &key : importConflicts) {
        assert(removalsAnswered.count(key) == 1 && "every import conflict has its ticked removal");
    }
    for (const auto &row : rowsOf(controller)) {
        if (row.section == Section::Conflicts && row.conflict
            && !(row.conflict->rekordboxChoice.empty() && row.conflict->engineChoice.empty())) {
            assert(row.resolvedSide == QStringLiteral("rekordbox"));
        }
    }
    std::cout << "  rekordbox's side for all: " << importConflicts.size() << " import conflicts, "
              << controller.conflictCount() << " left open, " << controller.checkedCount() << " ticked\n";
    controller.clearAllConflictResolutions();
    assert(tickedRemovalKeys().empty());
    assert(controller.conflictCount() == FixtureConflicts);
    assert(controller.checkedCount() == checkedBefore);
    controller.resolveAllConflicts(false);
    assert(tickedRemovalKeys().empty() && "Engine's side removes nothing");
    assert(controller.conflictCount() < FixtureConflicts);
    controller.clearAllConflictResolutions();
    assert(controller.conflictCount() == FixtureConflicts);
    std::cout << "case 6 (every conflict answered at once, and every answer cleared) OK\n";

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
           == QStringLiteral("Compared with how this stick looked when Seabass last saved it, export 15132."));
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

// 7. On a proposal built here, no stick: the order without conflicts and
//    with one, and what an add, a membership and a cue row say they do.
void testOrderAndDetails()
{
    namespace d = seabass::domain;
    using Cue = d::CuePoint;
    const auto header = [](const std::string &key, std::vector<std::string> dependsOn = {}) {
        d::EngineUpdateItemHeader h;
        h.key = key;
        h.checkedByDefault = true;
        h.reasonText = "test";
        h.dependsOn = std::move(dependsOn);
        return h;
    };
    const auto hot = [](int pad, double ms) {
        Cue cue;
        cue.kind = Cue::Kind::Hot;
        cue.hotCueNumber = pad;
        cue.positionMs = ms;
        return cue;
    };

    d::Track night;
    night.title = "Night Drive";
    night.artist = "Ana Example";
    night.filename = "night.mp3";
    night.filePath = "/stick/Contents/night.mp3";
    night.bpm = 124.0;
    night.key = "Am";
    night.durationSeconds = 391.4;
    night.rating = 4;
    night.comment = "warm up";
    night.artworkPath = "/stick/PIONEER/Artwork/00001/a1.jpg";
    Cue loop = hot(2, 30000);
    loop.isLoop = true;
    loop.loopEndMs = 34000;
    Cue memory;
    memory.positionMs = 60500;
    night.cues = {hot(1, 1000), loop, memory};
    night.playlists = {{"Warm Up", 1, 7}};

    d::Track second;
    second.title = "Second Song";
    second.filename = "second.mp3";
    second.filePath = "/stick/Contents/second.mp3";
    second.playlists = {{"Warm Up", 2, 7}};

    d::EngineUpdateProposal proposal;
    d::PlaylistCreate create;
    create.header = header("playlist|7");
    create.pdbId = 7;
    create.path = "Warm Up";
    proposal.playlistsToCreate.push_back(create);
    d::TrackToAdd add;
    add.header = header("track|contents/night.mp3");
    add.rekordbox = night;
    add.stickRelativePath = "Contents/night.mp3";
    add.pathKey = "contents/night.mp3";
    proposal.tracksToAdd.push_back(add);
    d::MembershipEdit first;
    first.header = header("member|7|contents/night.mp3", {"playlist|7", "track|contents/night.mp3"});
    first.pdbId = 7;
    first.playlistPath = "Warm Up";
    first.pathKey = "contents/night.mp3";
    first.track = night;
    d::MembershipEdit after = first;
    after.header = header("member|7|contents/second.mp3", {"playlist|7"});
    after.pathKey = "contents/second.mp3";
    after.track = second;
    after.afterPathKey = "contents/night.mp3";
    proposal.membership = {first, after};
    d::CueEdit cues;
    cues.header = header("cue|contents/second.mp3|pad1");
    cues.plan.direction = d::SyncPlan::Direction::ToB;
    cues.plan.match.trackA = second;
    cues.plan.match.trackB = second;
    cues.plan.match.trackB.cues = {hot(1, 1500), hot(3, 90000)};
    cues.plan.cuesToApply = {hot(1, 1000), hot(2, 5000)};
    cues.plan.positionToleranceMs = 50;
    cues.pathKey = "contents/second.mp3";
    proposal.cuesToEngine.push_back(cues);

    RekordboxExportSyncListModel model;
    model.setProposal(proposal, "/stick");
    assert(model.rowCount() == 5);
    // The legend's words: Night Drive added; Night Drive and Second Song
    // put into "Warm Up" (two tracks, each once); the playlist created.
    {
        const QVariantMap legend = RekordboxExportSyncListModel::legendTexts(model.summaryCounts());
        assert(legend.value("newTracks").toString() == QStringLiteral("1 track added"));
        assert(legend.value("newPlaylists").toString() == QStringLiteral("1 playlist created"));
        assert(legend.value("changed").toString()
               == QStringLiteral("2 tracks moved between playlists, 1 track's cues changed"));
        assert(legend.value("removed").toString().isEmpty() && legend.value("other").toString().isEmpty());
        QVariantMap some;
        some.insert(QStringLiteral("tracksRemoved"), 3);
        some.insert(QStringLiteral("playlistsRemoved"), 1);
        some.insert(QStringLiteral("membershipRemoves"), 1);
        some.insert(QStringLiteral("playlistsRenamed"), 2);
        some.insert(QStringLiteral("ratingsAndComments"), 2);
        const QVariantMap other = RekordboxExportSyncListModel::legendTexts(some);
        assert(other.value("removed").toString()
               == QStringLiteral("3 tracks removed, 1 playlist removed, 1 track taken out of a playlist"));
        assert(other.value("other").toString() == QStringLiteral("2 playlists renamed"));
        assert(other.value("changed").toString() == QStringLiteral("2 ratings and comments changed"));
        assert(other.value("newTracks").toString().isEmpty());
    }
    assert(model.index(0).data(RekordboxExportSyncListModel::SectionRole).toString() == QStringLiteral("playlists"));
    assert(model.index(0).data(RekordboxExportSyncListModel::SectionIndexRole).toInt() == 1);

    const auto rowOf = [&model](const QString &kind, int nth = 0) {
        for (int i = 0; i < model.rowCount(); ++i) {
            if (model.rows()[static_cast<std::size_t>(i)].kind == kind && nth-- == 0) {
                return i;
            }
        }
        return -1;
    };
    // The cue strips' data (CueSidesRole): a cue row has both copies,
    // rekordbox's first, and the one the save writes (Engine) says what it
    // gains and loses and holds the cues it will have.
    {
        const int cueRow = rowOf(QStringLiteral("cues"));
        const QVariantList sides = model.index(cueRow).data(RekordboxExportSyncListModel::CueSidesRole).toList();
        assert(sides.size() == 2);
        const QVariantMap rb = sides[0].toMap();
        const QVariantMap en = sides[1].toMap();
        std::cout << "  cue sides: " << rb.value("cueText").toString().toStdString() << " | "
                  << en.value("cueText").toString().toStdString() << "\n";
        assert(rb.value("written").toBool() == false);
        assert(en.value("written").toBool() == true);
        assert(rb.value("cues").toList().isEmpty());
        assert(rb.value("cueText").toString() == QStringLiteral("no cues"));
        const QVariantList now = en.value("cues").toList();
        assert(now.size() == 2);
        assert(now[0].toMap().value("hotCueNumber").toInt() == 1 && now[0].toMap().value("positionMs").toDouble() == 1500);
        assert(now[1].toMap().value("hotCueNumber").toInt() == 3 && now[1].toMap().value("positionMs").toDouble() == 90000);
        const QVariantList proposed = en.value("proposedCues").toList();
        assert(proposed.size() == 2);
        assert(proposed[0].toMap().value("hotCueNumber").toInt() == 1
               && proposed[0].toMap().value("positionMs").toDouble() == 1000);
        assert(proposed[1].toMap().value("hotCueNumber").toInt() == 2
               && proposed[1].toMap().value("positionMs").toDouble() == 5000);
        // Pad 1 at 0:01.500 becomes 0:01.000, pad 3 goes, pad 2 comes:
        // two gained, two lost.
        assert(en.value("cueText").toString() == QStringLiteral("2 hot · gains 2 · loses 2"));
        // A track to add: rekordbox's copy alone, its three cues.
        const QVariantList add = model.index(rowOf(QStringLiteral("addTrack")))
                                     .data(RekordboxExportSyncListModel::CueSidesRole).toList();
        assert(add.size() == 1 && add[0].toMap().value("cues").toList().size() == 3);
        assert(add[0].toMap().value("cueText").toString() == QStringLiteral("2 hot · 1 memory"));
        // A membership carries none.
        assert(model.index(rowOf(QStringLiteral("addMember"))).data(RekordboxExportSyncListModel::CueSidesRole)
                   .toList().isEmpty());
    }
    const auto expect = [&model](int row, const QStringList &lines) {
        const QStringList got = model.index(row).data(RekordboxExportSyncListModel::DetailsRole).toStringList();
        if (got != lines) {
            std::cerr << "details of row " << row << ":\n";
            for (const auto &line : got) {
                std::cerr << "  " << line.toStdString() << "\n";
            }
        }
        assert(got == lines);
        assert(model.details(row) == lines);
    };
    expect(rowOf(QStringLiteral("addTrack")),
           {QStringLiteral("Adds this track to Engine, as rekordbox has it:"), QStringLiteral("Title: Night Drive"),
            QStringLiteral("Artist: Ana Example"), QStringLiteral("File: Contents/night.mp3"),
            QStringLiteral("BPM: 124, key: Am, length: 6:31"), QStringLiteral("Rating: 4 stars"),
            QStringLiteral("Comment: \"warm up\""), QStringLiteral("Pad 1: 0:01.000"),
            QStringLiteral("Pad 2: loop 0:30.000 to 0:34.000"), QStringLiteral("Memory cue: 1:00.500"),
            QStringLiteral("Cover: yes"), QStringLiteral("Joins \"Warm Up\" #1")});
    expect(rowOf(QStringLiteral("addMember"), 1),
           {QStringLiteral("Puts Second Song into \"Warm Up\" on Engine"),
            QStringLiteral("Position in rekordbox: #2"), QStringLiteral("Goes after Night Drive by Ana Example"),
            QStringLiteral("If that track is left out, it goes at the end"),
            QStringLiteral("File: Contents/second.mp3")});
    expect(rowOf(QStringLiteral("cues")),
           {QStringLiteral("Writes Engine's cues:"), QStringLiteral("Pad 1: was 0:01.500, now 0:01.000"),
            QStringLiteral("Pad 2: new, 0:05.000"), QStringLiteral("Pad 3: 1:30.000, cleared")});
    // Covers: the add's is the rekordbox track's, as a local file URL; a
    // playlist row has no track and no cover; a cue row falls back on the
    // other side's copy.
    {
        const QModelIndex addAt = model.index(rowOf(QStringLiteral("addTrack")));
        assert(addAt.data(RekordboxExportSyncListModel::HasTrackRole).toBool());
        const QString url = addAt.data(RekordboxExportSyncListModel::ArtworkPathRole).toString();
        assert(url.startsWith(QStringLiteral("file:")));
        assert(QUrl(url).toLocalFile().toStdString() == night.artworkPath);
        const QModelIndex createAt = model.index(rowOf(QStringLiteral("createPlaylist")));
        assert(!createAt.data(RekordboxExportSyncListModel::HasTrackRole).toBool());
        assert(createAt.data(RekordboxExportSyncListModel::ArtworkPathRole).toString().isEmpty());
        const QModelIndex memberAt = model.index(rowOf(QStringLiteral("addMember")));
        assert(QUrl(memberAt.data(RekordboxExportSyncListModel::ArtworkPathRole).toString()).toLocalFile().toStdString()
               == night.artworkPath);
        assert(model.index(rowOf(QStringLiteral("cues"))).data(RekordboxExportSyncListModel::HasTrackRole).toBool());
    }
    // The overview bar's categories, by hand: the add, the create, two
    // membership adds and the cues.
    {
        const QVariantMap categories = model.categoryCounts();
        assert(categories.value("newTracks").toInt() == 1);
        assert(categories.value("newPlaylists").toInt() == 1);
        assert(categories.value("changed").toInt() == 3);
        assert(categories.value("removed").toInt() == 0);
        assert(categories.value("other").toInt() == 0);
    }
    // A playlist the add no longer joins in this save says so.
    model.setIncluded(rowOf(QStringLiteral("addMember")), false);
    assert(model.details(rowOf(QStringLiteral("addTrack"))).last() == QStringLiteral("Joins no playlist in this save"));

    // With a conflict, it heads the list.
    d::EngineUpdateConflict conflict;
    conflict.header = header("member|7|contents/third.mp3");
    conflict.header.checkedByDefault = false;
    conflict.header.conflict = true;
    conflict.rekordboxSide = "Take it out";
    conflict.engineSide = "Keep it";
    d::MembershipEdit takeOut = first;
    takeOut.kind = d::MembershipEdit::Kind::Remove;
    takeOut.track = second;
    takeOut.header.dependsOn.clear();
    conflict.rekordboxChoice = {takeOut};
    proposal.conflicts.push_back(conflict);
    model.setProposal(proposal, "/stick");
    assert(model.index(0).data(RekordboxExportSyncListModel::SectionRole).toString() == QStringLiteral("conflicts"));
    assert(model.index(0).data(RekordboxExportSyncListModel::SectionIndexRole).toInt() == 0);
    assert(model.index(1).data(RekordboxExportSyncListModel::SectionRole).toString() == QStringLiteral("playlists"));
    expect(0, {QStringLiteral("Why: test"), QStringLiteral("Take it out (rekordbox's side):"),
               QStringLiteral("  Takes Second Song out of \"Warm Up\" on Engine"), QStringLiteral("  Position on Engine: #2"),
               QStringLiteral("  The track stays in the library"), QStringLiteral("  File: Contents/second.mp3"),
               QStringLiteral("Keep it (Engine's side):"), QStringLiteral("  writes nothing; Engine keeps what it has")});
    // Not in the bar until answered; answered toward rekordbox it is a
    // membership remove, and removed.
    assert(model.categoryCounts().value("removed").toInt() == 0);
    assert(model.resolveConflict(0, true));
    assert(model.categoryCounts().value("removed").toInt() == 1);
    assert(model.categoryCounts().value("changed").toInt() == 3);
    std::cout << "case 7 (conflicts first, playlists first without them; details of an add, a membership, cues "
                 "and a conflict) OK\n";
}

// 8. See the top of the file.
void testUnansweredOrderOnFirstRun(const fs::path &fixture)
{
    const auto stick = testing::makeEngineChangeStick(fixture, "export_sync_unanswered_order");
    RekordboxExportSyncController controller;
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    LibraryEditSession *session = sessionOf(stick);
    const auto uncheckAll = [&] {
        for (const char *section : {"playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                                    "cuesToEngine", "restoresToRekordbox"}) {
            controller.setSectionIncluded(QString::fromLatin1(section), false);
        }
        assert(controller.checkedCount() == 0);
    };

    // The fixture's one new playlist and its members, onto Engine.
    uncheckAll();
    const int create = firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist"));
    const Row createRow = rowsOf(controller)[static_cast<std::size_t>(create)];
    const std::string playlistPath = std::get<seabass::domain::PlaylistCreate>(*createRow.edit).path;
    const std::uint32_t pdbId = std::get<seabass::domain::PlaylistCreate>(*createRow.edit).pdbId;
    std::vector<std::string> memberKeys;
    for (int m : rowsDependingOn(controller, createRow.header.key)) {
        controller.setIncluded(m, true);
        memberKeys.push_back(std::get<seabass::domain::MembershipEdit>(*rowsOf(controller)[static_cast<std::size_t>(m)].edit).pathKey);
    }
    assert(memberKeys.size() >= 3);
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });

    // No record, and Engine's first and last member swapped.
    fs::remove(testing::baselineFileOf(stick));
    const auto list = testing::sqlRows(stick.db, "SELECT id FROM Playlist WHERE parentListId = 0 AND title = '"
                                                     + playlistPath + "';");
    assert(list.size() == 1);
    const auto tracks = testing::rootPlaylistTracks(stick.db, playlistPath);
    assert(tracks.size() == memberKeys.size());
    const std::string first = std::to_string(tracks.front());
    const std::string last = std::to_string(tracks.back());
    const std::string where = " WHERE listId = " + list[0][0] + " AND trackId = ";
    assert(testing::sqlExec(stick.db, "UPDATE PlaylistEntity SET trackId = -1" + where + first + ";"
                                          "UPDATE PlaylistEntity SET trackId = " + first + where + last + ";"
                                          "UPDATE PlaylistEntity SET trackId = " + last + where + "-1;"));

    const auto orderRows = [&](seabass::domain::EngineUpdateReason reason) {
        int n = 0;
        for (const auto &row : rowsOf(controller)) {
            if (row.section == Section::Conflicts && row.header.reason == reason) {
                ++n;
            }
        }
        return n;
    };
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    assert(!controller.hasBaseline());
    printCounts(controller);
    // One question for the playlist, titled by it, every move under
    // rekordbox's side: the swap moves two members, each a remove and an
    // add. The fixture's 1245 and this one.
    assert(orderRows(seabass::domain::EngineUpdateReason::NoBaselineOrder) == 1
           && "the swap is one order question on a run without a record");
    assert(controller.conflictCount() == FixtureConflicts + 1);
    for (int i = 0; i < controller.rows()->rowCount(); ++i) {
        const Row &row = rowsOf(controller)[static_cast<std::size_t>(i)];
        if (row.header.reason != seabass::domain::EngineUpdateReason::NoBaselineOrder) {
            continue;
        }
        assert(row.header.key == seabass::domain::orderItemKey(pdbId));
        assert(row.title == QString::fromStdString(playlistPath) && !row.hasTrack);
        assert(row.conflict->rekordboxChoice.size() == 4);
        const QStringList lines = controller.rows()->details(i);
        assert(lines.count(QStringLiteral("Put \"%1\" in rekordbox's order (2 tracks move) (rekordbox's side):")
                               .arg(QString::fromStdString(playlistPath)))
               == 1);
        assert(lines.filter(QStringLiteral("  Takes ")).size() == 2 && lines.filter(QStringLiteral("  Puts ")).size() == 2);
    }

    // Saved with every question unanswered and every change unticked:
    // only the record is written.
    uncheckAll();
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    assert(session->pendingCount() == 1);
    saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });

    const auto baseline = testing::readBaseline(stick);
    assert(baseline);
    const auto *recorded = baseline->findPlaylist(pdbId);
    assert(recorded && recorded->members == memberKeys && "every member, in rekordbox's order");

    printCounts(controller);
    assert(controller.hasBaseline());
    assert(orderRows(seabass::domain::EngineUpdateReason::NoBaselineOrder) == 0);
    assert(orderRows(seabass::domain::EngineUpdateReason::BothChanged) == 0);
    bool keptAsEngines = false;
    for (const auto &row : rowsOf(controller)) {
        keptAsEngines = keptAsEngines
            || (row.section == Section::EngineOwnKept
                && row.header.reasonText
                    == "Engine changed the order of \"" + playlistPath + "\" after Seabass last recorded the stick");
    }
    assert(keptAsEngines);
    std::cout << "case 8 (an order question left unanswered without a record: recorded as rekordbox has it, then "
                 "Engine's own) OK\n";
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

    testOrderAndDetails();
    testProposalTicksAndAnswers(fixture);
    testSmallSave(fixture);
    testUnansweredOrderOnFirstRun(fixture);
    std::cout << "rekordbox_export_sync_controller_test: all cases OK\n";
    return 0;
}
