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
// 8. An order difference on a run without a record is no question: a
//    playlist saved onto Engine, its record taken away and two of its
//    Engine members swapped gives Engine rekordbox's order, checked, every
//    move under the playlist's order item. A save that leaves them
//    unticked records rekordbox's order with every member, so the next
//    analysis keeps Engine's order as its own.
// 9. Only what the user unticked is declined: the new playlist's create
//    unticked takes its members with it, and the record declines the
//    create alone. When rekordbox later changes the playlist (its declined
//    state no longer matches) the create comes back ticked, and so do its
//    members.
// 10. What a rating write onto rekordbox says it does: a restore of
//    Seabass's own write (ExportDropped) puts it back; an answer toward
//    Engine of a conflict nothing recorded the origin of (OriginUnknown)
//    sets Engine's, and does not call it Seabass's.
//
// 13. #126: a reused id among rekordbox playlists sharing one path: the old
//     playlist offered for deletion and deleted (testReusedAtASharedPath).
//
// argv[1]: tests/fixtures/anonymized_library.

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/save_context.hpp"
#include "engine_change_fixture.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/rekordbox_export_sync_controller.hpp"
#include "gui/rekordbox_export_sync_list_model.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/track_matching.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "nothing_else_running.hpp"
#include "rekordbox_baseline_fixture.hpp"
#include "scratch_path.hpp"

using seabass::pathFromUtf8;
using seabass::pathToUtf8;
using seabass::gui::EditSessionRegistry;
using seabass::gui::LibraryEditSession;
using seabass::gui::PendingChange;
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
// The planner's conflicts, and the rows they are asked as: the tracks
// Engine imported that rekordbox no longer lists go one row per Engine
// playlist (RekordboxExportSyncListModel's importedRowGroups()).
constexpr int FixtureConflicts = 1245;
constexpr int FixtureConflictRows = 959;
// Engine's own, without the playlists only Engine has, which have their
// own section.
constexpr int FixtureKept = 1160;
constexpr int FixtureEngineOnlyPlaylists = 17;
// One fewer since 2026-10-17: the track whose only difference was Engine's
// cue point, which is not a memory cue to copy onto rekordbox any more.
constexpr int FixtureChecked = 952;

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
    std::cout << "  conflict rows " << count(controller, "conflicts") << "\n";
    assert(count(controller, "conflicts") == FixtureConflictRows);
    assert(controller.conflictCount() == FixtureConflictRows);
    {
        // Every conflict under exactly one row, and a group never of one.
        std::set<std::string> keys;
        int conflicts = 0;
        for (const auto &row : rowsOf(controller)) {
            if (row.section != Section::Conflicts) {
                continue;
            }
            assert(row.group.empty() || row.group.size() >= 2);
            for (const auto *conflict : RekordboxExportSyncListModel::conflictsOf(row)) {
                ++conflicts;
                keys.insert(conflict->header.key);
            }
        }
        assert(conflicts == FixtureConflicts);
        assert(static_cast<int>(keys.size()) == FixtureConflicts);
    }
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
        // 925 membership adds, 1 rating or comment, 17 cue rows, 8 restores
        // (9 until 2026-10-17: one was Engine's cue point as a memory cue).
        assert(categories.value("changed").toInt() == 951);
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
        assert(legend.value("changed").toString() == QStringLiteral("779 tracks moved between playlists, 1 rating or comment changed, 17 tracks' cues changed, 8 restores onto rekordbox"));
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
            if (imported < 0 && rowsOf(controller)[static_cast<std::size_t>(i)].conflict
                && rowsOf(controller)[static_cast<std::size_t>(i)].header.reason
                    == seabass::domain::EngineUpdateReason::NoBaselineImportedRow) {
                imported = i;
            }
        }
        // An imported row rekordbox no longer lists, on a row of its own.
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
    std::cout << "case 1 (the fixture's proposal: 1 create, 925 members, 1245 conflicts on 959 rows, 1160 kept, 17 Engine-only playlists, 953 ticked) OK\n";

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
        if (row.section == Section::Conflicts && row.conflict
            && row.header.reason == seabass::domain::EngineUpdateReason::NoBaselineImportedRow) {
            conflict = static_cast<int>(i);
            break;
        }
    }
    assert(conflict >= 0 && "an import conflict on a row of its own");
    const std::string conflictKey = rowsOf(controller)[static_cast<std::size_t>(conflict)].header.key;
    const int removalsBefore = count(controller, "tracksToRemove");
    const int checkedBefore = controller.checkedCount();
    controller.resolveConflict(conflict, true);
    assert(count(controller, "tracksToRemove") == removalsBefore + 1);
    assert(controller.conflictCount() == FixtureConflictRows - 1);
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
    assert(controller.conflictCount() == FixtureConflictRows - 1);
    assert(controller.rows()->index(conflict).data(RekordboxExportSyncListModel::ResolvedSideRole).toString()
           == QStringLiteral("engine"));
    controller.clearConflictResolution(conflict);
    assert(controller.conflictCount() == FixtureConflictRows);
    assert(controller.checkedCount() == checkedBefore);
    std::cout << "case 3 (an import conflict answered: a removal toward rekordbox, nothing toward Engine) OK\n";

    // 3b. A playlist's worth of them, one row: answered toward rekordbox,
    //     one removal per track under that track's own key; toward Engine,
    //     nothing; taken back, the list as it was.
    {
        int groupRow = -1;
        for (std::size_t i = 0; i < rowsOf(controller).size(); ++i) {
            if (!rowsOf(controller)[i].group.empty()) {
                groupRow = static_cast<int>(i);
                break;
            }
        }
        assert(groupRow >= 0);
        const Row &group = rowsOf(controller)[static_cast<std::size_t>(groupRow)];
        std::set<std::string> memberKeys;
        for (const auto &member : group.group) {
            memberKeys.insert(member.header.key);
        }
        const QModelIndex at = controller.rows()->index(groupRow);
        assert(at.data(RekordboxExportSyncListModel::IsConflictRole).toBool());
        assert(at.data(RekordboxExportSyncListModel::RekordboxChoiceLabelRole).toString()
               == QStringLiteral("Remove these %1 tracks from Engine").arg(memberKeys.size()));
        assert(at.data(RekordboxExportSyncListModel::EngineChoiceLabelRole).toString() == QStringLiteral("Keep them in Engine"));
        const QStringList lines = at.data(RekordboxExportSyncListModel::DetailsRole).toStringList();
        assert(lines.front().startsWith(QStringLiteral("Why: ")));
        assert(lines.contains(QStringLiteral("Tracks:")));
        assert(lines.size() == 6 + static_cast<int>(memberKeys.size()) && "one line a track, no field lists");
        controller.resolveConflict(groupRow, true);
        assert(count(controller, "tracksToRemove") == removalsBefore + static_cast<int>(memberKeys.size()));
        assert(controller.conflictCount() == FixtureConflictRows - 1);
        std::set<std::string> removed;
        for (const auto &row : rowsOf(controller)) {
            if (row.section == Section::TracksToRemove && row.fromConflictUid >= 0) {
                assert(row.included);
                removed.insert(row.header.key);
            }
        }
        assert(removed == memberKeys);
        controller.resolveConflict(groupRow, false);
        assert(count(controller, "tracksToRemove") == removalsBefore);
        controller.clearConflictResolution(groupRow);
        assert(controller.conflictCount() == FixtureConflictRows);
        assert(controller.checkedCount() == checkedBefore);
    }
    std::cout << "case 3b (a playlist of import conflicts answered as one row) OK\n";

    // 6. Every conflict at once, the one answered above included.
    controller.resolveConflict(conflict, false);
    std::set<std::string> importConflicts;
    for (const auto &row : rowsOf(controller)) {
        if (row.section == Section::Conflicts
            && row.header.reason == seabass::domain::EngineUpdateReason::NoBaselineImportedRow) {
            for (const auto *member : RekordboxExportSyncListModel::conflictsOf(row)) {
                importConflicts.insert(member->header.key);
            }
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
        if (row.section != Section::Conflicts) {
            continue;
        }
        const auto members = RekordboxExportSyncListModel::conflictsOf(row);
        if (std::any_of(members.begin(), members.end(), [](const auto *c) {
                return !(c->rekordboxChoice.empty() && c->engineChoice.empty());
            })) {
            assert(row.resolvedSide == QStringLiteral("rekordbox"));
        }
    }
    std::cout << "  rekordbox's side for all: " << importConflicts.size() << " import conflicts, "
              << controller.conflictCount() << " left open, " << controller.checkedCount() << " ticked\n";
    controller.clearAllConflictResolutions();
    assert(tickedRemovalKeys().empty());
    assert(controller.conflictCount() == FixtureConflictRows);
    assert(controller.checkedCount() == checkedBefore);
    controller.resolveAllConflicts(false);
    assert(tickedRemovalKeys().empty() && "Engine's side removes nothing");
    assert(controller.conflictCount() < FixtureConflictRows);
    controller.clearAllConflictResolutions();
    assert(controller.conflictCount() == FixtureConflictRows);
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
void testOrderOnFirstRun(const fs::path &fixture)
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
            if (row.header.reason == reason) {
                assert(row.section == Section::Membership && "an order is never a conflict");
                ++n;
            }
        }
        return n;
    };
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    assert(!controller.hasBaseline());
    printCounts(controller);
    // No question: the swap moves two members, each a remove and an add,
    // checked, all under the playlist's order item. The fixture's
    // conflicts and nothing more.
    assert(orderRows(seabass::domain::EngineUpdateReason::NoBaselineOrder) == 4
           && "the swap is four checked moves on a run without a record");
    assert(controller.conflictCount() == FixtureConflictRows);
    // The new playlist's members are on Engine now, and the order's four
    // moves join the rest, ticked: 925 - 14 + 4 = 915, 953 - 1 - 14 + 4.
    const int saved = static_cast<int>(memberKeys.size());
    assert(count(controller, "membership") == FixtureMembership - saved + 4);
    assert(controller.checkedCount() == FixtureChecked - FixtureCreates - saved + 4);
    int takes = 0;
    int puts = 0;
    for (int i = 0; i < controller.rows()->rowCount(); ++i) {
        const Row &row = rowsOf(controller)[static_cast<std::size_t>(i)];
        if (row.header.reason != seabass::domain::EngineUpdateReason::NoBaselineOrder) {
            continue;
        }
        assert(row.header.key == seabass::domain::orderItemKey(pdbId));
        assert(row.included && row.header.checkedByDefault && !row.header.conflict);
        assert(row.header.reasonText == "The two orders differ (2 tracks move); Engine takes rekordbox's order");
        const auto &edit = std::get<seabass::domain::MembershipEdit>(*row.edit);
        assert(edit.playlistPath == playlistPath);
        (edit.kind == seabass::domain::MembershipEdit::Kind::Remove ? takes : puts) += 1;
    }
    assert(takes == 2 && puts == 2);

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
    std::cout << "case 8 (an order without a record: rekordbox's, checked; left unticked, recorded as rekordbox "
                 "has it, then Engine's own) OK\n";
}

// 9. See the top of the file.
void testDeclinedByDependency(const fs::path &fixture)
{
    const auto stick = testing::makeEngineChangeStick(fixture, "export_sync_declined_by_dependency");
    RekordboxExportSyncController controller;
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    LibraryEditSession *session = sessionOf(stick);
    for (const char *section : {"playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                                "cuesToEngine", "restoresToRekordbox"}) {
        controller.setSectionIncluded(QString::fromLatin1(section), false);
    }
    assert(controller.checkedCount() == 0);

    // The user unticks the create only; its members go with it.
    const int create = firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist"));
    const std::string createKey = rowsOf(controller)[static_cast<std::size_t>(create)].header.key;
    std::vector<std::string> memberItems;
    for (int m : rowsDependingOn(controller, createKey)) {
        memberItems.push_back(rowsOf(controller)[static_cast<std::size_t>(m)].header.key);
        controller.setIncluded(m, true);
    }
    assert(memberItems.size() >= 3);
    assert(included(controller, create));
    controller.setIncluded(create, false);
    for (int m : rowsDependingOn(controller, createKey)) {
        assert(!included(controller, m));
    }
    assert(controller.checkedCount() == 0);
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    assert(session->pendingCount() == 1 && "only the record");
    saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });

    auto baseline = testing::readBaseline(stick);
    assert(baseline);
    assert(baseline->declined.count(createKey) == 1 && "the create the user unticked is declined");
    int membersDeclined = 0;
    for (const auto &key : memberItems) {
        membersDeclined += static_cast<int>(baseline->declined.count(key));
    }
    std::cout << "  members declined with the create: " << membersDeclined << " of " << memberItems.size() << "\n";
    assert(membersDeclined == 0 && "a member unticked only with its create is not declined");

    // rekordbox renames the playlist: the create's declined state no
    // longer matches, so it is offered again.
    baseline->declined[createKey] = "renamed in rekordbox since";
    testing::plantBaseline(stick, *baseline);
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    const int again = firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist"));
    assert(again >= 0 && rowsOf(controller)[static_cast<std::size_t>(again)].header.key == createKey);
    assert(included(controller, again) && "the create comes back ticked");
    const std::vector<int> back = rowsDependingOn(controller, createKey);
    std::cout << "  members offered again: " << back.size() << " of " << memberItems.size() << "\n";
    assert(back.size() == memberItems.size() && "its members come back with it");
    for (int m : back) {
        assert(included(controller, m) && "ticked, as the create is");
    }
    std::cout << "case 9 (a row unticked with its dependency is not declined: the create comes back with its "
                 "members) OK\n";
}

// 12. Issue #99 on a copy of the fixture's stick: rekordbox gives a
//     deleted playlist's id to a new, unrelated one. The record says id N
//     was "Reused Old" holding tracks Q, and Engine has "Reused Old"
//     holding exactly Q; rekordbox's id N is "Playlist 006", which holds
//     none of Q. The page deletes "Reused Old" and creates "Playlist 006",
//     two items (the delete under a key of the plan's own), never renames
//     the one into the other. Saved:
//     a: everything ticked: "Playlist 006" holds rekordbox's tracks in
//        rekordbox's order;
//     b: the whole Membership section unticked: it holds nothing;
//     c: the old playlist's delete unticked, the rest ticked: Engine
//        keeps "Reused Old";
//     d: the create unticked, the delete ticked: Engine has neither;
//     e: both unticked: Engine untouched.
//     Every time: none of Q under the new name, the record holds no
//     "Reused Old" and nothing but the file's key grammar, the next
//     analysis offers nothing under the id, and a "Reused Old" left on
//     Engine is listed as Engine's own playlist. Then (where Engine has
//     "Playlist 006") rekordbox puts one of Q into it: the page offers
//     the add, ticked, and nothing says Engine took it out (the defect
//     the reviews left open: the old playlist's tracks recorded under
//     the new one).
//     f: as a, the record also declining a rename of "Reused Old" to
//        "Playlist 006" (a decline of the old item that matches the new
//        create's state): the create is offered all the same, ticked, and
//        no decline under the id survives the save.
//     g: every decline the first save recorded kept, and one more: the
//        add of one of "Playlist 006"'s tracks into the old playlist
//        (rekordbox's old playlist held it). Not certain any more that the
//        two are unrelated, nor that they are one: "Playlist 006" keeps
//        one of the four tracks, so a conflict under the id (#125), never
//        a rename, nothing saved.
//     Every case that ticks both: the old playlist's delete is staged
//        directly before the new one's create (stageSelected), so no
//        stopped save lands the create without the delete.
//     h: as a, the DJ's Cancel arriving while "Reused Old" is deleted:
//        the delete lands, the create does not. No warning; Engine has
//        neither playlist; the record holds nothing under the id; the
//        next analysis offers the create, ticked, with every member's add,
//        no conflict and nothing saying Engine took a track out. Staged
//        and saved, the record is a's.
//     j: as h, one of "Playlist 006"'s members unticked: the next analysis
//        offers the create and the other adds; that one stays out, its
//        decline made about this same playlist.
//     i: as a, a cancel arriving after a change staged before both halves
//        landed: neither half on the stick, the record keeps "Reused Old"
//        with Q and exactly its own declines under the id (one planted),
//        and the next analysis offers the delete and the create again.
//     k: as i, one of "Playlist 006"'s members unticked.
//     l: as i, the create unticked (the delete ticked).
enum class ReusedSave {
    Members,
    Bare,
    OldLeft,
    NewLeft,
    Declined,
    RenameDeclined,
    MemberDeclined,
    CancelAfterDelete,
    CancelBeforeBoth,
    CancelAfterDeleteMemberUnticked,
    CancelBeforeBothMemberUnticked,
    CancelBeforeBothCreateUnticked
};

// A staged change, forwarded, that stops the save once it has landed
// (stopAfter) the way the DJ's Cancel arrives while it is written, through
// the save's own token. Everything else is the staged change's own.
class Forwarded final : public PendingChange
{
public:
    Forwarded(std::shared_ptr<PendingChange> staged, bool stopAfter)
        : m_staged(std::move(staged)), m_stopAfter(stopAfter)
    {
    }
    QString id() const override { return m_staged->id(); }
    QString description() const override { return m_staged->description(); }
    QString subject() const override { return m_staged->subject(); }
    QString unit() const override { return m_staged->unit(); }
    int unitsWritten() const override { return m_staged->unitsWritten(); }
    int unitsSkipped() const override { return m_staged->unitsSkipped(); }
    QString verb() const override { return m_staged->verb(); }
    QString playlistEdited() const override { return m_staged->playlistEdited(); }
    QStringList unitKeys() const override { return m_staged->unitKeys(); }
    QStringList formatsTouched() const override { return m_staged->formatsTouched(); }
    QString owner() const override { return m_staged->owner(); }
    std::vector<seabass::gui::BackupTarget> filesToBackup(seabass::gui::SaveContext &ctx) const override
    {
        return m_staged->filesToBackup(ctx);
    }
    std::vector<seabass::gui::RekordboxWrite> rekordboxWrites() const override { return m_staged->rekordboxWrites(); }
    void beforeSave(seabass::gui::SaveContext &ctx) override { m_staged->beforeSave(ctx); }
    seabass::gui::ChangeOutcome apply(seabass::gui::SaveContext &ctx) override
    {
        const auto outcome = m_staged->apply(ctx);
        if (m_stopAfter) {
            const_cast<seabass::application::CancellationToken &>(ctx.cancel()).cancel();
        }
        return outcome;
    }

private:
    std::shared_ptr<PendingChange> m_staged;
    bool m_stopAfter = false;
};

// Writes nothing and stops the save once it has landed: a change staged
// before everything else, which the cancel lets land.
class LandsThenStops final : public PendingChange
{
public:
    QString id() const override { return QStringLiteral("rekordbox-export-sync:test-stop"); }
    QString description() const override { return QStringLiteral("stops the save"); }
    QString unit() const override { return QStringLiteral("changes"); }
    QStringList formatsTouched() const override { return {}; }
    seabass::gui::ChangeOutcome apply(seabass::gui::SaveContext &ctx) override
    {
        const_cast<seabass::application::CancellationToken &>(ctx.cancel()).cancel();
        return seabass::gui::ChangeOutcome::success();
    }
};

void testReusedPlaylistId(const fs::path &fixture, ReusedSave how)
{
    const char *const names[] = {"export_sync_reused_id",          "export_sync_reused_id_bare",
                                 "export_sync_reused_id_old",      "export_sync_reused_id_new",
                                 "export_sync_reused_id_declined", "export_sync_reused_id_rename_declined",
                                 "export_sync_reused_id_member_declined", "export_sync_reused_id_cancel_after_delete",
                                 "export_sync_reused_id_cancel_before_both", "export_sync_reused_id_cancel_after_delete_member",
                                 "export_sync_reused_id_cancel_member_unticked",
                                 "export_sync_reused_id_cancel_create_unticked"};
    const char *const letters[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};
    const bool afterDelete = how == ReusedSave::CancelAfterDelete || how == ReusedSave::CancelAfterDeleteMemberUnticked;
    const bool beforeBoth = how == ReusedSave::CancelBeforeBoth || how == ReusedSave::CancelBeforeBothMemberUnticked
        || how == ReusedSave::CancelBeforeBothCreateUnticked;
    const bool cancelled = afterDelete || beforeBoth;
    const bool all = how == ReusedSave::Members || how == ReusedSave::RenameDeclined || cancelled;
    const bool tickDelete = all || how == ReusedSave::Bare || how == ReusedSave::NewLeft;
    const bool tickCreate =
        (all && how != ReusedSave::CancelBeforeBothCreateUnticked) || how == ReusedSave::Bare || how == ReusedSave::OldLeft;
    const bool tickMembers = (all && how != ReusedSave::CancelBeforeBothCreateUnticked) || how == ReusedSave::OldLeft;
    namespace d = seabass::domain;
    namespace engine = seabass::infrastructure::engine;
    const auto stick =
        testing::makeEngineChangeStick(fixture, names[static_cast<int>(how)]);
    const std::string letter = letters[static_cast<int>(how)];
    const std::string newName = "Playlist 006";
    const std::string oldName = "Reused Old";
    const std::string enginePath = pathToUtf8(stick.engine);
    const char *sections[] = {"playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                              "cuesToEngine", "restoresToRekordbox"};

    // A record of the stick as it stands: a save with nothing ticked.
    RekordboxExportSyncController controller;
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    LibraryEditSession *session = sessionOf(stick);
    for (const char *section : sections) {
        controller.setSectionIncluded(QString::fromLatin1(section), false);
    }
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
    auto baseline = testing::readBaseline(stick);
    assert(baseline);
    d::BaselinePlaylist *reused = nullptr;
    for (auto &playlist : baseline->playlists) {
        if (playlist.path == newName) {
            reused = &playlist;
        }
    }
    assert(reused && !reused->folder);

    // Both catalogs, paired as the planner pairs them. rekordbox's
    // "Playlist 006" from export.pdb (the record holds only the members
    // that were level), and Q: three tracks both catalogs list that it
    // does not hold, by their Engine rows.
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader rekordboxReader(pathToUtf8(stick.pioneer));
    const std::vector<d::Track> rekordbox = rekordboxReader.readAll();
    engine::LibdjinteropEngineReader engineReader(enginePath);
    const std::vector<d::Track> engineTracks = seabass::application::ScanLibrary(engineReader).execute();
    std::map<std::string, std::int64_t> engineIdByKey;
    for (const auto &[r, e] : d::matchTracks(rekordbox, engineTracks, d::MatchScope::OneStick)) {
        engineIdByKey[testing::keyOf(stick, *r)] = std::stoll(e->sourceId);
    }
    std::vector<std::pair<int, const d::Track *>> placed;
    for (const auto &track : rekordbox) {
        for (const auto &membership : track.playlists) {
            if (membership.name == newName) {
                placed.emplace_back(membership.position, &track);
            }
        }
    }
    std::sort(placed.begin(), placed.end());
    assert(!placed.empty() && "the fixture's playlist has tracks");
    std::set<std::string> rekordboxSet;
    std::vector<std::string> rekordboxFiles;  // its tracks' file names, in its order
    for (const auto &entry : placed) {
        rekordboxSet.insert(testing::keyOf(stick, *entry.second));
        rekordboxFiles.push_back(pathToUtf8(pathFromUtf8(entry.second->filePath).filename()));
    }
    std::vector<std::string> q;
    for (const auto &entry : engineIdByKey) {
        if (!rekordboxSet.count(entry.first) && q.size() < 3) {
            q.push_back(entry.first);
        }
    }
    assert(q.size() == 3 && "the fixture has tracks for the old playlist");

    const std::uint32_t id = reused->id;
    const std::string key = "playlist:" + std::to_string(id);
    reused->path = oldName;
    reused->members = q;
    // Nothing of id N declined, so the page offers what it plans for it
    // (but f, which declines the old item's rename to the new name, g,
    // whose one decline under the id is a member's, and i to l, which
    // plant an order's).
    for (auto it = baseline->declined.begin(); it != baseline->declined.end();) {
        const bool ofReused = it->first == key || it->first == "order:" + std::to_string(id)
            || it->first.rfind("member:" + std::to_string(id) + ":", 0) == 0;
        it = ofReused ? baseline->declined.erase(it) : std::next(it);
    }
    if (beforeBoth) {
        baseline->declined["order:" + std::to_string(id)] = "planted for the old playlist";
    }
    if (how == ReusedSave::RenameDeclined) {
        baseline->declined[key] = d::engineUpdateStateHash("list:" + newName);
    }
    if (how == ReusedSave::MemberDeclined) {
        baseline->declined["member:" + std::to_string(id) + ":" + *rekordboxSet.begin()] = "the add, declined";
    }
    testing::plantBaseline(stick, *baseline);

    // Engine: its "Playlist 006" becomes "Reused Old", holding Q.
    assert(testing::sqlExec(stick.db, "UPDATE Playlist SET title = '" + oldName + "' WHERE title = '" + newName
                                          + "' AND parentListId = 0;"));
    for (const std::int64_t track : testing::rootPlaylistTracks(stick.db, oldName)) {
        engine::removeFromEnginePlaylist(enginePath, oldName, track);
    }
    for (const auto &member : q) {
        engine::addToEnginePlaylist(enginePath, oldName, engineIdByKey.at(member));
    }
    assert(testing::rootPlaylistTracks(stick.db, oldName).size() == q.size());

    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    if (how == ReusedSave::MemberDeclined) {
        // Not certainly two playlists (#99), and rekordbox's keeps one of
        // the four tracks rekordbox's old playlist held: not certainly the
        // same either, so a conflict under the id, never a guess (#125).
        bool asked = false;
        for (const auto &row : rowsOf(controller)) {
            const auto *rename = row.edit ? std::get_if<d::PlaylistRename>(&*row.edit) : nullptr;
            assert(!(rename && rename->fromPath == oldName) && "never a rename on a guess");
            const auto *c = row.edit ? std::get_if<d::PlaylistCreate>(&*row.edit) : nullptr;
            if (c && c->path == newName) {
                std::cerr << "FAIL: a playlist rekordbox held one of its tracks in is taken as unrelated\n";
                assert(false);
            }
            asked = asked || (row.section == Section::Conflicts && row.header.key == key);
        }
        assert(asked && "not certain either way: a conflict under the id");
        std::cout << "case 12g (#99, #125: the record's one decline under the id says rekordbox's old playlist held "
                     "a new track, one of four kept: a conflict) OK\n";
        return;
    }
    int create = -1;
    int del = -1;
    std::string oldKey;
    {
        bool renamed = false;
        const auto &rows = rowsOf(controller);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!rows[i].edit) {
                continue;
            }
            if (const auto *rename = std::get_if<d::PlaylistRename>(&*rows[i].edit)) {
                std::cerr << "  the page renames \"" << rename->fromPath << "\" to \"" << rename->toPath << "\"\n";
                renamed = renamed || rename->fromPath == oldName;
            }
            if (const auto *c = std::get_if<d::PlaylistCreate>(&*rows[i].edit); c && c->path == newName) {
                create = static_cast<int>(i);
            }
            if (const auto *e = std::get_if<d::PlaylistDelete>(&*rows[i].edit); e && e->path == oldName) {
                del = static_cast<int>(i);
            }
            if (const auto *m = std::get_if<d::MembershipEdit>(&*rows[i].edit)) {
                assert(m->playlistPath != oldName && "no row of its own for the old playlist's tracks");
            }
        }
        assert(!renamed && "no rename of the old playlist into the new one");
        if (create < 0 && how == ReusedSave::RenameDeclined) {
            std::cerr << "FAIL: the create of \"" << newName << "\" is suppressed by the old playlist's decline\n";
        }
        assert(create >= 0 && del >= 0 && "the old playlist deleted, the new one created");
        assert(rows[static_cast<std::size_t>(create)].included && "the create is offered ticked");
        assert(rows[static_cast<std::size_t>(create)].header.key == key);
        oldKey = rows[static_cast<std::size_t>(del)].header.key;
        assert(oldKey == "reused:" + std::to_string(id) + ":" + oldName && "two items: the delete keyed by the plan");
        assert(!d::parseItemKey(oldKey));
    }

    for (const char *section : sections) {
        controller.setSectionIncluded(QString::fromLatin1(section), false);
    }
    assert(!included(controller, create) && !included(controller, del));
    if (tickCreate) {
        controller.setIncluded(create, true);
        assert(!included(controller, del) && "the delete is an item of its own");
    }
    if (tickDelete) {
        controller.setIncluded(del, true);
        assert(included(controller, create) == tickCreate && "the create is an item of its own");
    }
    if (tickMembers) {
        for (int m : rowsDependingOn(controller, key)) {
            controller.setIncluded(m, true);
        }
    }
    if (how == ReusedSave::CancelBeforeBothMemberUnticked || how == ReusedSave::CancelAfterDeleteMemberUnticked) {
        const auto members = rowsDependingOn(controller, key);
        assert(!members.empty());
        controller.setIncluded(members.front(), false);
    }
    // What the next analysis may list under the id: nothing.
    const auto underTheId = [&]() {
        int count = 0;
        for (const auto &row : rowsOf(controller)) {
            if (row.header.key == key || row.header.key == oldKey
                || row.header.key.rfind("member:" + std::to_string(id) + ":", 0) == 0) {
                std::cerr << "  listed again: " << row.kind.toStdString() << " " << row.header.key << "\n";
                ++count;
            }
        }
        return count;
    };
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    // The staging order: the old playlist's delete directly before the new
    // one's create, whenever both are ticked.
    int createAt = -1;
    int deleteAt = -1;
    {
        const auto &pending = session->pendingChanges();
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (pending[i]->unitKeys().contains(QString::fromStdString(key)) && createAt < 0) {
                createAt = static_cast<int>(i);
            }
            if (pending[i]->unitKeys().contains(QString::fromStdString(oldKey))) {
                deleteAt = static_cast<int>(i);
            }
        }
        assert((deleteAt >= 0) == tickDelete && (createAt >= 0) == tickCreate);
        if (tickDelete && tickCreate && createAt != deleteAt + 1) {
            std::cerr << "FAIL: the old playlist's delete is staged at " << deleteAt << ", the create at " << createAt
                      << "\n";
        }
        assert((!tickDelete || !tickCreate || createAt == deleteAt + 1)
               && "the old playlist's delete directly before the new one's create");
    }
    if (cancelled) {
        // The staged changes, each forwarded, in the session's order: the
        // stop after the delete (h, j), or a change before all of them (i).
        const std::vector<std::shared_ptr<PendingChange>> staged = session->pendingChanges();
        QStringList ids;
        for (const auto &change : staged) {
            ids << change->id();
        }
        session->unstageAll(ids);
        std::vector<std::unique_ptr<PendingChange>> restaged;
        if (beforeBoth) {
            restaged.push_back(std::make_unique<LandsThenStops>());
        }
        for (std::size_t i = 0; i < staged.size(); ++i) {
            const bool stop = afterDelete && static_cast<int>(i) == deleteAt;
            restaged.push_back(std::make_unique<Forwarded>(staged[i], stop));
        }
        assert(session->stageAll(std::move(restaged)));
        const QVariantMap first =
            saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
        std::cout << "  cancelled save: error \"" << first.value("error").toString().toStdString() << "\", warning \""
                  << first.value("warning").toString().toStdString() << "\"\n";
        assert(first.value("cancelled").toBool() && first.value("error").toString().isEmpty());
        const auto record = testing::readBaseline(stick);
        assert(record);
        const auto *under = record->findPlaylist(id);
        if (beforeBoth) {
            // Neither half landed: the record as before, the old playlist
            // whole with exactly its own declines under the id, and both
            // halves offered again.
            assert(testing::rootPlaylistTracks(stick.db, oldName).size() == q.size());
            assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + newName + "';") == 0);
            if (!under || under->path != oldName) {
                std::cerr << "FAIL: the record holds id " << id << " as "
                          << (under ? "\"" + under->path + "\"" : std::string("nothing")) << "\n";
            }
            assert(under && under->path == oldName && under->members == q);
            assert(first.value("warning").toString().isEmpty());
            std::map<std::string, std::string> declinedUnderTheId;
            for (const auto &[declinedKey, state] : record->declined) {
                if (declinedKey == key || declinedKey == "order:" + std::to_string(id)
                    || declinedKey.rfind("member:" + std::to_string(id) + ":", 0) == 0) {
                    declinedUnderTheId.emplace(declinedKey, state);
                }
            }
            if (declinedUnderTheId.size() != 1) {
                for (const auto &[declinedKey, state] : declinedUnderTheId) {
                    std::cerr << "  the record declines under the id: " << declinedKey << "\n";
                }
            }
            assert((declinedUnderTheId
                    == std::map<std::string, std::string>{
                        {"order:" + std::to_string(id), "planted for the old playlist"}})
                   && "exactly the previous record's declines under the id");
            bool createAgain = false;
            bool deleteAgain = false;
            for (const auto &row : rowsOf(controller)) {
                createAgain = createAgain || (row.header.key == key && row.kind == QStringLiteral("createPlaylist"));
                deleteAgain = deleteAgain || row.header.key == oldKey;
            }
            if (!createAgain || !deleteAgain) {
                for (const auto &row : rowsOf(controller)) {
                    std::cerr << "  listed: " << row.kind.toStdString() << " " << row.header.key << "\n";
                }
            }
            assert(createAgain && deleteAgain && "both halves offered again");
            session->unstageAll(session->pendingIds());
            std::cout << "case 12" << letter << " (#99: a cancel before both halves keeps the old playlist in the "
                      << "record, both offered again) OK\n";
            return;
        }
        // The delete landed, the create did not: nothing to warn about,
        // nothing under the id in the record.
        assert(first.value("warning").toString().isEmpty());
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + oldName + "';") == 0);
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + newName + "';") == 0);
        if (under) {
            std::cerr << "FAIL: the record holds id " << id << " as \"" << under->path << "\"\n";
        }
        assert(!under && !record->findPlaylistByPath(oldName));
        // The next analysis: the create, ticked, and its members' adds.
        bool createOffered = false;
        int adds = 0;
        for (const auto &row : rowsOf(controller)) {
            const bool ofTheId =
                row.header.key == key || row.header.key.rfind("member:" + std::to_string(id) + ":", 0) == 0;
            if (!ofTheId) {
                continue;
            }
            std::cout << "  then lists: " << row.kind.toStdString() << " " << row.header.key << ": "
                      << row.header.reasonText << "\n";
            assert(row.section != Section::Conflicts && "no conflict");
            assert(row.header.reasonText.find("took this track out") == std::string::npos);
            assert(row.header.key != oldKey);
            if (row.kind == QStringLiteral("createPlaylist")) {
                createOffered = row.included;
            } else if (row.kind == QStringLiteral("addMember") && row.included) {
                ++adds;
            }
        }
        assert(createOffered && "the create offered again, ticked");
        const int wantAdds = static_cast<int>(rekordboxFiles.size())
            - (how == ReusedSave::CancelAfterDeleteMemberUnticked ? 1 : 0);
        if (adds != wantAdds) {
            std::cerr << "FAIL: " << adds << " member adds offered, not " << wantAdds << "\n";
        }
        assert(adds == wantAdds);
        if (how == ReusedSave::CancelAfterDeleteMemberUnticked) {
            std::cout << "case 12j (#99: a cancel after the delete, a member unticked: the create offered again, "
                         "that member's decline kept for this playlist) OK\n";
            return;
        }
        controller.stageSelected();
        assert(controller.errorMessage().isEmpty());
    }
    const QVariantMap summary =
        saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
    assert(summary.value("error").toString().isEmpty());
    assert(summary.value("warning").toString().isEmpty());

    const auto oldLeft = testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + oldName + "';");
    const auto newMade = testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + newName + "';");
    assert(oldLeft == (tickDelete ? 0 : 1));
    assert(newMade == (tickCreate ? 1 : 0));
    if (!tickDelete) {
        assert(testing::rootPlaylistTracks(stick.db, oldName).size() == q.size() && "Engine's old playlist untouched");
    }
    if (tickCreate) {
        const auto holds = testing::rootPlaylistTracks(stick.db, newName);
        for (const auto &member : q) {
            assert(std::find(holds.begin(), holds.end(), engineIdByKey.at(member)) == holds.end()
                   && "none of the old playlist's tracks under the new name");
        }
        if (tickMembers) {
            // Every track of rekordbox's list, in its order.
            std::vector<std::string> files;
            for (const std::int64_t track : holds) {
                const auto path =
                    testing::sqlRows(stick.db, "SELECT path FROM Track WHERE id = " + std::to_string(track) + ";");
                assert(path.size() == 1);
                files.push_back(pathToUtf8(pathFromUtf8(path[0][0]).filename()));
            }
            std::cout << "  \"" << newName << "\" holds " << files.size() << " of rekordbox's " << rekordboxFiles.size()
                      << " tracks\n";
            assert(files == rekordboxFiles);
        } else {
            assert(holds.empty());
        }
    }

    // The record: rekordbox's playlist under the id or nothing, never the
    // old one or its tracks, and only keys of the file's grammar.
    {
        const auto record = testing::readBaseline(stick);
        assert(record);
        const auto *under = record->findPlaylist(id);
        assert(!under == !tickCreate);
        if (under) {
            assert(under->path == newName);
            for (const auto &member : q) {
                if (std::find(under->members.begin(), under->members.end(), member) != under->members.end()) {
                    std::cerr << "FAIL: the record holds " << member << " under \"" << newName << "\"\n";
                    assert(false);
                }
            }
        }
        assert(!record->findPlaylistByPath(oldName));
        for (const auto &[declinedKey, state] : record->declined) {
            if (!d::parseItemKey(declinedKey)) {
                std::cerr << "FAIL: the record holds the key \"" << declinedKey << "\"\n";
                assert(false);
            }
            if (all && (declinedKey == key || declinedKey == "order:" + std::to_string(id)
                        || declinedKey.rfind("member:" + std::to_string(id) + ":", 0) == 0)) {
                std::cerr << "FAIL: everything saved, and the record still declines " << declinedKey << "\n";
                assert(false);
            }
        }
    }

    // The page analysed the stick again after the save.
    assert(underTheId() == 0 && "the record is right: nothing under the id again");
    {
        bool listedAsEngines = false;
        for (const auto &row : rowsOf(controller)) {
            assert(row.section != Section::Conflicts || row.title.toStdString().find(oldName) == std::string::npos);
            if (row.section == Section::EngineOnlyPlaylists && row.title.toStdString() == oldName) {
                listedAsEngines = true;
            }
        }
        assert(listedAsEngines == !tickDelete && "left on Engine, the old playlist is Engine's own");
    }

    if (tickCreate) {
        // rekordbox puts the first of Q into "Playlist 006".
        const auto *q0 = baseline->findTrack(q[0]);
        assert(q0 && q0->pdbId != 0);
        {
            seabass::infrastructure::rekordbox::PdbRowWriter pdb(pathToUtf8(stick.pioneer / "rekordbox" / "export.pdb"));
            assert(pdb.appendPlaylistEntries(id, {q0->pdbId}) == 1);
            assert(pdb.commit());
        }
        analyze(controller, stick);
        assert(controller.errorMessage().isEmpty());
        int add = -1;
        const auto &rows = rowsOf(controller);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const Row &row = rows[i];
            if (row.header.key != "member:" + std::to_string(id) + ":" + q[0]) {
                continue;
            }
            std::cout << "  after rekordbox's add: " << row.kind.toStdString() << ": " << row.header.reasonText << "\n";
            assert(row.header.reasonText.find("took this track out") == std::string::npos
                   && "nothing says Engine took it out");
            const auto *m = row.edit ? std::get_if<d::MembershipEdit>(&*row.edit) : nullptr;
            if (m && m->kind == d::MembershipEdit::Kind::Add && m->playlistPath == newName) {
                add = static_cast<int>(i);
            }
        }
        assert(add >= 0 && included(controller, add) && "the add is offered, ticked");
    }
    std::cout << "case 12" << letter << " (#99: a reused rekordbox playlist id, delete " << (tickDelete ? "ticked" : "unticked")
              << ", create " << (tickCreate ? "ticked" : "unticked") << ", members "
              << (tickMembers ? "ticked" : "unticked") << ") OK\n";
}

// A staged change that cancels the save once it has landed, the way the
// DJ's Cancel arrives while it is written. Everything else is the staged
// change's own.
class CancelsOnceLanded final : public PendingChange
{
public:
    CancelsOnceLanded(std::shared_ptr<PendingChange> staged, seabass::application::CancellationToken &token)
        : m_staged(std::move(staged)), m_token(token)
    {
    }
    QString id() const override { return m_staged->id(); }
    QString description() const override { return m_staged->description(); }
    QString subject() const override { return m_staged->subject(); }
    QString unit() const override { return m_staged->unit(); }
    int unitsWritten() const override { return m_staged->unitsWritten(); }
    int unitsSkipped() const override { return m_staged->unitsSkipped(); }
    QString verb() const override { return m_staged->verb(); }
    QString playlistEdited() const override { return m_staged->playlistEdited(); }
    QStringList unitKeys() const override { return m_staged->unitKeys(); }
    QStringList formatsTouched() const override { return m_staged->formatsTouched(); }
    QString owner() const override { return m_staged->owner(); }
    std::vector<seabass::gui::BackupTarget> filesToBackup(seabass::gui::SaveContext &ctx) const override
    {
        return m_staged->filesToBackup(ctx);
    }
    std::vector<seabass::gui::RekordboxWrite> rekordboxWrites() const override { return m_staged->rekordboxWrites(); }
    void beforeSave(seabass::gui::SaveContext &ctx) override { m_staged->beforeSave(ctx); }
    seabass::gui::ChangeOutcome apply(seabass::gui::SaveContext &ctx) override
    {
        const auto outcome = m_staged->apply(ctx);
        m_token.cancel();
        return outcome;
    }

private:
    std::shared_ptr<PendingChange> m_staged;
    seabass::application::CancellationToken &m_token;
};

// The staged changes saved as the session would, with a cancel arriving
// while changes[cancelAt] is written.
seabass::gui::SaveLoopResult saveCancelledAt(const testing::EngineChangeStick &stick,
                                             std::vector<std::shared_ptr<PendingChange>> changes, std::size_t cancelAt)
{
    seabass::application::CancellationToken token;
    changes[cancelAt] = std::make_shared<CancelsOnceLanded>(changes[cancelAt], token);
    seabass::gui::SaveContext ctx(token, seabass::application::NullProgressReporter::instance(), {},
                                  stick.pioneerPath(), stick.enginePath());
    return seabass::gui::runSaveLoop(changes, ctx);
}

// 11. A cancel never splits what one staged change of this page shares
//     with another (PendingChange::unitKeys()): a playlist's create and
//     its members, a track added to Engine and the playlist it goes into,
//     a conflict's chosen cues and the track's other cue write, staged
//     last. Saved with the cancel arriving, on the stick: what a cancel
//     lets land must land, not fail for want of what it depends on.
void testCancelUnits(const fs::path &fixture)
{
    const auto untickAll = [](RekordboxExportSyncController &controller) {
        for (const char *section : {"playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                                    "cuesToEngine", "restoresToRekordbox"}) {
            controller.setSectionIncluded(QString::fromLatin1(section), false);
        }
        assert(controller.checkedCount() == 0);
    };
    const auto stagedChanges = [](RekordboxExportSyncController &controller, const testing::EngineChangeStick &stick) {
        controller.stageSelected();
        if (!controller.errorMessage().isEmpty()) {
            std::cerr << controller.errorMessage().toStdString() << "\n";
        }
        assert(controller.errorMessage().isEmpty());
        return sessionOf(stick)->pendingChanges();
    };
    const auto leafOf = [](const std::string &path) {
        return path.rfind('/') == std::string::npos ? path : path.substr(path.rfind('/') + 1);
    };

    // A playlist's create and its members, and cues of a track that is
    // in none of it. A cancel while the playlist is created lets its
    // members land and not the cues: the playlist is on Engine, whole.
    {
        const auto stick = testing::makeEngineChangeStick(fixture, "export_sync_cancel_units");
        RekordboxExportSyncController controller;
        analyze(controller, stick);
        assert(controller.errorMessage().isEmpty());
        untickAll(controller);
        const int create = firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist"));
        const Row createRow = rowsOf(controller)[static_cast<std::size_t>(create)];
        const std::string playlistPath = std::get<seabass::domain::PlaylistCreate>(*createRow.edit).path;
        std::set<std::string> memberTracks;
        for (int m : rowsDependingOn(controller, createRow.header.key)) {
            controller.setIncluded(m, true);
            memberTracks.insert(
                std::get<seabass::domain::MembershipEdit>(*rowsOf(controller)[static_cast<std::size_t>(m)].edit).pathKey);
        }
        assert(!memberTracks.empty());
        bool cues = false;
        for (std::size_t i = 0; i < rowsOf(controller).size() && !cues; ++i) {
            const Row &row = rowsOf(controller)[i];
            if (row.section == Section::CuesToEngine && row.fromConflictUid < 0
                && !memberTracks.count(std::get<seabass::domain::CueEdit>(*row.edit).pathKey)) {
                controller.setIncluded(static_cast<int>(i), true);
                cues = true;
            }
        }
        assert(cues);
        const auto changes = stagedChanges(controller, stick);
        assert(changes.size() == 4 && "the create, the members, the cues, the record");
        const auto result = saveCancelledAt(stick, changes, 0);
        if (!result.error.isEmpty()) {
            std::cerr << result.error.toStdString() << "\n";
        }
        assert(result.cancelled && result.error.isEmpty());
        assert(result.appliedIds == (QStringList{changes[0]->id(), changes[1]->id()})
               && "the create and its members land, the cues do not");
        assert(testing::rootPlaylistTracks(stick.db, leafOf(playlistPath)).size() == memberTracks.size()
               && "the playlist is on Engine with every member");
        sessionOf(stick)->unstageAll(sessionOf(stick)->pendingIds());
    }

    // A track added to Engine and the membership that holds it. The
    // fixture's Engine has every track, so a member of the playlist the
    // analysis creates is taken out of it first (and its file put on the
    // stick, which the fixture has no audio for) for the analysis to add
    // back. A cancel while it is added lets the membership land.
    {
        const auto stick = testing::makeEngineChangeStick(fixture, "export_sync_cancel_units_add");
        RekordboxExportSyncController controller;
        analyze(controller, stick);
        assert(controller.errorMessage().isEmpty());
        std::string playlistPath;
        {
            const int create = firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist"));
            const Row &createRow = rowsOf(controller)[static_cast<std::size_t>(create)];
            playlistPath = std::get<seabass::domain::PlaylistCreate>(*createRow.edit).path;
            const auto members = rowsDependingOn(controller, createRow.header.key);
            assert(!members.empty());
            const auto &member =
                std::get<seabass::domain::MembershipEdit>(*rowsOf(controller)[static_cast<std::size_t>(members.front())].edit);
            const std::string name = pathToUtf8(pathFromUtf8(member.track.filePath).filename());
            assert(testing::sqlExec(stick.db, "DELETE FROM Track WHERE path LIKE '%/" + name + "';"));
            const fs::path file = pathFromUtf8(member.track.filePath);
            fs::create_directories(file.parent_path());
            // Adding it reads its sample rate, so it is a real MP3: a few
            // seconds of silent MPEG-1 Layer III frames, 128 kbit/s at
            // 44.1 kHz (417 bytes each).
            std::ofstream mp3(file, std::ios::binary);
            for (int frame = 0; frame < 200; ++frame) {
                std::string bytes(417, '\0');
                bytes[0] = '\xFF';
                bytes[1] = '\xFB';
                bytes[2] = '\x90';
                bytes[3] = '\x00';
                mp3 << bytes;
            }
        }
        analyze(controller, stick);
        assert(controller.errorMessage().isEmpty());
        untickAll(controller);
        const int add = firstRow(controller, Section::TracksToAdd);
        assert(add >= 0 && "the analysis adds the track back");
        const std::vector<int> holding = rowsDependingOn(controller, rowsOf(controller)[static_cast<std::size_t>(add)].header.key);
        assert(!holding.empty() && "and a membership holds it");
        controller.setIncluded(add, true);
        for (int m : holding) {
            controller.setIncluded(m, true);
        }
        const auto changes = stagedChanges(controller, stick);
        // The add is the first change that holds the track's key: the
        // memberships after it depend on it, the create before it does not.
        const QString trackKey = QString::fromStdString(rowsOf(controller)[static_cast<std::size_t>(add)].header.key);
        std::size_t addAt = changes.size();
        for (std::size_t i = 0; i < changes.size(); ++i) {
            if (changes[i]->unitKeys().contains(trackKey)) {
                addAt = i;
                break;
            }
        }
        assert(addAt + 1 < changes.size());
        const auto result = saveCancelledAt(stick, changes, addAt);
        if (!result.error.isEmpty()) {
            std::cerr << result.error.toStdString() << "\n";
        }
        assert(result.cancelled && result.error.isEmpty());
        assert(result.appliedIds.size() == static_cast<qsizetype>(changes.size() - 1)
               && "everything but the record lands: the playlist, the add and its membership");
        assert(testing::rootPlaylistTracks(stick.db, leafOf(playlistPath)).size() == holding.size()
               && "the added track is in its playlist");
        sessionOf(stick)->unstageAll(sessionOf(stick)->pendingIds());
    }

    // A conflict's chosen cues and the track's other cue write, and a
    // membership of a playlist inside a renamed folder. The fixture has
    // neither (no record for two sides to have moved from, no folder to
    // rename), so the rows are built here and asked of the rule the
    // staging uses.
    {
        const auto cueRow = [](const std::string &key, const std::string &pathKey) {
            Row row;
            row.section = Section::CuesToEngine;
            row.header.key = key;
            seabass::domain::CueEdit cues;
            cues.pathKey = pathKey;
            row.edit = cues;
            return row;
        };
        Row answer = cueRow("cue|contents/night.mp3|2", "contents/night.mp3");
        answer.fromConflictUid = 3;
        Row rename;
        rename.section = Section::Playlists;
        rename.header.key = "playlist|10";
        rename.edit = seabass::domain::PlaylistRename{};
        Row member;
        member.section = Section::Membership;
        member.header.key = "member|11|contents/day.mp3";
        member.header.dependsOn = {"playlist|10", "playlist|11"};
        member.edit = seabass::domain::MembershipEdit{};
        const std::vector<Row> rows{cueRow("cue|contents/night.mp3|1", "contents/night.mp3"),
                                    cueRow("cue|contents/day.mp3|1", "contents/day.mp3"), answer, rename, member};
        const auto shares = [&rows](std::size_t a, std::size_t b) {
            const QStringList left = RekordboxExportSyncController::unitKeysOf(rows, {a});
            const QStringList right = RekordboxExportSyncController::unitKeysOf(rows, {b});
            return std::any_of(left.begin(), left.end(), [&right](const QString &key) { return right.contains(key); });
        };
        assert(shares(0, 2) && "the chosen cues are the track's unit, staged apart from its other cue write");
        assert(!shares(0, 1) && "another track's cues are not");
        assert(shares(3, 4) && "a membership lands with the rename of the folder above its playlist");
    }
    std::cout << "case 11 (a cancel lands a create with its members and an add with its playlist, and links a "
                 "conflict's answer to its track and a member to its folder's rename) OK\n";
}

// 15. #128, on the page's model alone: a folder "F" under an open
//     conflict P (rekordbox's side creates it, Engine's renames "Old" to
//     it), a conflict C about "F/G" inside it, and two new playlists "F/H"
//     and "F/I" that wait on P.
//     a: C answered while P is open: its row says it waits for P, not
//        answered with nothing written; P answered, C's answer is ticked.
//     b: P answered, F/H unticked by the DJ; P switched to Engine's side,
//        then taken back and answered again: F/H stays out, F/I ticked.
void testWaitingAnswers()
{
    namespace d = seabass::domain;
    const auto header = [](const std::string &key, std::vector<std::string> dependsOn, bool conflict) {
        d::EngineUpdateItemHeader h;
        h.key = key;
        h.checkedByDefault = !conflict;
        h.conflict = conflict;
        h.reasonText = "test";
        h.dependsOn = std::move(dependsOn);
        return h;
    };
    const auto create = [&](const std::string &key, std::uint32_t id, const std::string &path,
                            std::vector<std::string> dependsOn) {
        d::PlaylistCreate c;
        c.header = header(key, std::move(dependsOn), false);
        c.pdbId = id;
        c.path = path;
        return c;
    };
    d::EngineUpdateProposal proposal;
    d::EngineUpdateConflict p;
    p.header = header("playlist:1", {}, true);
    p.rekordboxSide = "Create \"F\"";
    p.engineSide = "Rename \"Old\" to \"F\"";
    p.rekordboxChoice.emplace_back(create("playlist:1", 1, "F", {}));
    d::PlaylistRename rename;
    rename.header = header("playlist:1", {}, false);
    rename.pdbId = 1;
    rename.fromPath = "Old";
    rename.toPath = "F";
    p.engineChoice.emplace_back(rename);
    proposal.conflicts.push_back(p);
    d::EngineUpdateConflict c;
    c.header = header("playlist:2", {"playlist:1"}, true);
    c.rekordboxSide = "Create \"F/G\"";
    c.engineSide = "Leave Engine without \"F/G\"";
    c.rekordboxChoice.emplace_back(create("playlist:2", 2, "F/G", {"playlist:1"}));
    proposal.conflicts.push_back(c);
    proposal.playlistsToCreate.push_back(create("playlist:3", 3, "F/H", {"playlist:1"}));
    proposal.playlistsToCreate.push_back(create("playlist:4", 4, "F/I", {"playlist:1"}));

    RekordboxExportSyncListModel model;
    model.setProposal(proposal, "/stick");
    const auto at = [&](const std::string &key, bool conflictRow) {
        for (std::size_t i = 0; i < model.rows().size(); ++i) {
            const Row &row = model.rows()[i];
            if (row.header.key == key && (row.section == Section::Conflicts) == conflictRow) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const auto ticked = [&](const std::string &key) {
        const int i = at(key, false);
        return i >= 0 && model.rows()[static_cast<std::size_t>(i)].included;
    };
    const auto direction = [&](const std::string &key) {
        return model.index(at(key, true)).data(RekordboxExportSyncListModel::DirectionRole).toString().toStdString();
    };
    assert(!ticked("playlist:3") && !ticked("playlist:4") && "held back while P is open");

    // a: C answered first.
    assert(model.resolveConflict(at("playlist:2", true), true));
    assert(!ticked("playlist:2") && "C's create waits for P");
    const QStringList details = model.index(at("playlist:2", true)).data(RekordboxExportSyncListModel::DetailsRole).toStringList();
    std::cout << "  C answered while P is open: [" << direction("playlist:2") << "] "
              << (details.isEmpty() ? std::string() : details.front().toStdString()) << "\n";
    if (direction("playlist:2") != "waits") {
        std::cerr << "FAIL (a): C reads \"" << direction("playlist:2") << "\" while nothing of its answer is written\n";
    }
    assert(direction("playlist:2") == "waits");
    assert(!details.isEmpty() && details.front().startsWith(QStringLiteral("Waits for an answer to ")));
    assert(model.resolveConflict(at("playlist:1", true), true));
    assert(ticked("playlist:1") && ticked("playlist:2") && "P answered: C's answer goes with it");
    assert(direction("playlist:2") == "to Engine");
    assert(ticked("playlist:3") && ticked("playlist:4"));

    // b: F/H unticked by the DJ, then P switched, taken back, answered.
    assert(model.setIncluded(at("playlist:3", false), false));
    assert(!ticked("playlist:3") && ticked("playlist:4"));
    assert(model.resolveConflict(at("playlist:1", true), false));
    if (ticked("playlist:3")) {
        std::cerr << "FAIL (b): switching P's answer ticks F/H again, which the DJ unticked\n";
    }
    assert(!ticked("playlist:3") && ticked("playlist:4"));
    model.setIncluded(at("playlist:1", true), false);  // taken back
    assert(!ticked("playlist:3") && !ticked("playlist:4"));
    assert(model.resolveConflict(at("playlist:1", true), true));
    assert(!ticked("playlist:3") && ticked("playlist:4") && "F/H stays out");
    std::cout << "case 15 (#128: an answer waiting on an open conflict says so; switching an answer keeps what the "
                 "DJ unticked out) OK\n";
}

// 10. See the top of the file.
void testMetadataWords()
{
    namespace d = seabass::domain;
    d::MetadataEdit edit;
    edit.field = d::MetadataEdit::Field::Rating;
    edit.direction = d::MetadataEdit::Direction::ToRekordbox;
    edit.rekordbox.title = "Night Drive";
    edit.engine.title = "Night Drive";
    edit.rating = 3;
    // The planner says which it is (MetadataEdit::restoresSeabassWrite);
    // the reason alone no longer decides the words.
    edit.header.reason = d::EngineUpdateReason::ExportDropped;
    edit.restoresSeabassWrite = true;
    const QString restore = RekordboxExportSyncController::describeMetadataWrite({edit}, false);
    edit.header.reason = d::EngineUpdateReason::OriginUnknown;
    edit.restoresSeabassWrite = false;
    const QString overwrite = RekordboxExportSyncController::describeMetadataWrite({edit}, false);
    d::MetadataEdit comment = edit;
    comment.field = d::MetadataEdit::Field::Comment;
    comment.comment = "warm up";
    const QString both = RekordboxExportSyncController::describeMetadataWrite({edit, comment}, false);
    std::cout << "  restore: " << restore.toStdString() << "\n  overwrite: " << overwrite.toStdString()
              << "\n  both: " << both.toStdString() << "\n";
    assert(restore == QStringLiteral("Put the rating Seabass wrote back on \"Night Drive\" in rekordbox"));
    assert(overwrite == QStringLiteral("Set the rating of \"Night Drive\" in rekordbox to 3 star(s), Engine's")
           && "nothing recorded that Seabass wrote it");
    assert(both == QStringLiteral("Set the rating and comment of \"Night Drive\" in rekordbox to Engine's"));
    edit.direction = d::MetadataEdit::Direction::ToEngine;
    assert(RekordboxExportSyncController::describeMetadataWrite({edit}, true)
           == QStringLiteral("Set the rating of \"Night Drive\" in Engine to 3 star(s)"));
    std::cout << "case 10 (a rating onto rekordbox: Seabass's put back, or Engine's set over it) OK\n";
}

// 8. canStageChanged follows ticks and answers, as the page's button needs
//    (#97). And with a record on the stick, an empty answer to rekordbox
//    playlists sharing one path (RekordboxPathShared) is not something to
//    stage: the planner raises that conflict from the libraries whatever
//    the record says, so a save of it would change nothing. An empty
//    answer the record does settle is testshark_t73_test insync's.
void testEmptyAnswerWithARecord(const fs::path &fixture)
{
    const auto stick = testing::makeEngineChangeStick(fixture, "export_sync_empty_answer");
    RekordboxExportSyncController controller;
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty() && !controller.hasBaseline());
    QSignalSpy canStageChanged(&controller, &RekordboxExportSyncController::canStageChanged);
    const int someRow = firstRow(controller, Section::Playlists, QStringLiteral("createPlaylist"));
    assert(someRow >= 0);
    controller.setIncluded(someRow, false);
    assert(canStageChanged.size() > 0 && "unticking a row is told");
    canStageChanged.clear();
    controller.setIncluded(someRow, true);
    assert(canStageChanged.size() > 0 && "ticking a row is told");

    // A first save of nothing ticked: the record, and with it the
    // conflicts the planner can ask again on top of a record.
    for (const char *section : {"playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                                "cuesToEngine", "restoresToRekordbox"}) {
        controller.setSectionIncluded(QString::fromLatin1(section), false);
    }
    controller.stageSelected();
    LibraryEditSession *session = sessionOf(stick);
    const QVariantMap first =
        saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
    assert(first.value("error").toString().isEmpty());
    assert(controller.hasBaseline());
    printCounts(controller);

    // A conflict whose one side writes nothing.
    int row = -1;
    bool rekordboxSide = false;
    const auto &rows = rowsOf(controller);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].section != Section::Conflicts) {
            continue;
        }
        const auto conflicts = RekordboxExportSyncListModel::conflictsOf(rows[i]);
        std::cout << "  conflict " << rows[i].header.key << ":";
        for (const auto *c : conflicts) {
            std::cout << " rekordbox " << (c->rekordboxChoice.empty() ? "empty" : "writes") << ", engine "
                      << (c->engineChoice.empty() ? "empty" : "writes") << ";";
        }
        std::cout << " (" << rows[i].header.reasonText << ")\n";
        const bool engineWritesNothing = std::all_of(conflicts.begin(), conflicts.end(), [](const auto *c) {
            return c->engineChoice.empty() && !c->rekordboxChoice.empty();
        });
        const bool rekordboxWritesNothing = std::all_of(conflicts.begin(), conflicts.end(), [](const auto *c) {
            return c->rekordboxChoice.empty() && !c->engineChoice.empty();
        });
        if (row < 0 && !conflicts.empty() && (engineWritesNothing || rekordboxWritesNothing)) {
            row = static_cast<int>(i);
            rekordboxSide = rekordboxWritesNothing;
        }
    }
    assert(row >= 0 && "precondition: a conflict one of whose sides writes nothing");
    const std::string conflictKey = rows[static_cast<std::size_t>(row)].header.key;
    std::cout << "  answering " << conflictKey << " with " << (rekordboxSide ? "rekordbox's" : "Engine's")
              << " side, which writes nothing\n";
    assert(controller.checkedCount() == 0);
    assert(!controller.canStage() && "precondition: nothing ticked, nothing answered");

    canStageChanged.clear();
    controller.resolveConflict(row, rekordboxSide);
    assert(canStageChanged.size() > 0 && "an answer is told");
    assert(controller.checkedCount() == 0 && "the answer writes nothing, so it ticks nothing");
    assert(rows[static_cast<std::size_t>(row)].header.reason
           == seabass::domain::EngineUpdateReason::RekordboxPathShared);
    assert(!controller.canStage() && "an empty answer the record cannot settle is nothing to stage");
    canStageChanged.clear();
    controller.clearConflictResolution(row);
    assert(canStageChanged.size() > 0 && "taking the answer back is told");
    assert(!controller.canStage());
    controller.resolveConflict(row, rekordboxSide);
    controller.stageSelected();
    assert(session->pendingCount() == 0 && "nothing staged for it");
    std::cout << "case 8 (canStageChanged follows ticks and answers; an empty answer to playlists sharing a path "
                 "is nothing to stage) OK\n";
}

}  // namespace

// 13. #126 on a copy of the fixture's stick: rekordbox spells one path for
//     several playlists (the shared-path conflict), and the record holds
//     one of them, one with tracks, as "Reused Shared Old" with a track
//     none of the group holds, as Engine has it: rekordbox gave that
//     playlist's id to a member of the group. The page lists the old
//     playlist's delete under the plan's own key, ticked, beside the
//     group's conflict; saved alone, Engine and the record lose "Reused
//     Shared Old", and the next analysis offers nothing of it.
void testReusedAtASharedPath(const fs::path &fixture)
{
    namespace d = seabass::domain;
    namespace engine = seabass::infrastructure::engine;
    const auto stick = testing::makeEngineChangeStick(fixture, "export_sync_reused_shared");
    const std::string oldName = "Reused Shared Old";
    const std::string enginePath = pathToUtf8(stick.engine);
    const char *sections[] = {"playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                              "cuesToEngine", "restoresToRekordbox"};

    // A record of the stick as it stands: a save with nothing ticked.
    RekordboxExportSyncController controller;
    analyze(controller, stick);
    LibraryEditSession *session = sessionOf(stick);
    for (const char *section : sections) {
        controller.setSectionIncluded(QString::fromLatin1(section), false);
    }
    controller.stageSelected();
    saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
    auto baseline = testing::readBaseline(stick);
    assert(baseline);

    // A shared path, and a recorded member of its group that holds tracks.
    std::string sharedPath;
    for (const auto &row : rowsOf(controller)) {
        if (row.section == Section::Conflicts && row.header.reason == d::EngineUpdateReason::RekordboxPathShared) {
            for (const auto *c : RekordboxExportSyncListModel::conflictsOf(row)) {
                for (const auto &edit : c->rekordboxChoice) {
                    if (const auto *create = std::get_if<d::PlaylistCreate>(&edit)) {
                        sharedPath = create->path;
                    } else if (const auto *m = std::get_if<d::MembershipEdit>(&edit)) {
                        sharedPath = m->playlistPath;
                    }
                }
            }
        }
        if (!sharedPath.empty()) {
            break;
        }
    }
    assert(!sharedPath.empty() && "the precondition: the fixture has rekordbox playlists sharing a path");
    std::set<std::string> groupMembers;
    d::BaselinePlaylist *reused = nullptr;
    for (auto &playlist : baseline->playlists) {
        if (playlist.path == sharedPath) {
            groupMembers.insert(playlist.members.begin(), playlist.members.end());
            if (!playlist.folder && !playlist.members.empty() && !reused) {
                reused = &playlist;
            }
        }
    }
    assert(reused && "the precondition: a recorded member of the group holds tracks");
    // Track keys both catalogs list, by Engine row.
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader rekordboxReader(pathToUtf8(stick.pioneer));
    const std::vector<d::Track> rekordbox = rekordboxReader.readAll();
    engine::LibdjinteropEngineReader engineReader(enginePath);
    const std::vector<d::Track> engineTracks = seabass::application::ScanLibrary(engineReader).execute();
    std::map<std::string, std::int64_t> engineIdByKey;
    for (const auto &[r, e] : d::matchTracks(rekordbox, engineTracks, d::MatchScope::OneStick)) {
        engineIdByKey[testing::keyOf(stick, *r)] = std::stoll(e->sourceId);
    }
    std::string q;
    for (const auto &entry : engineIdByKey) {
        if (!groupMembers.count(entry.first)) {
            q = entry.first;
            break;
        }
    }
    assert(!q.empty());
    const std::uint32_t id = reused->id;
    reused->path = oldName;
    reused->parentId = 0;
    reused->members = {q};
    for (auto it = baseline->declined.begin(); it != baseline->declined.end();) {
        const auto item = d::parseItemKey(it->first);
        it = item && item->playlistId == id ? baseline->declined.erase(it) : std::next(it);
    }
    testing::plantBaseline(stick, *baseline);
    assert(engine::createEnginePlaylist(enginePath, oldName) > 0);
    assert(engine::addToEnginePlaylist(enginePath, oldName, engineIdByKey.at(q)));

    analyze(controller, stick);
    const std::string oldKey = d::reusedPlaylistItemKey(id, oldName);
    int del = -1;
    bool sharedConflict = false;
    const auto &rows = rowsOf(controller);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].header.key == oldKey) {
            del = static_cast<int>(i);
        }
        sharedConflict = sharedConflict
            || (rows[i].section == Section::Conflicts
                && rows[i].header.reason == d::EngineUpdateReason::RekordboxPathShared);
        if (rows[i].edit) {
            if (const auto *rename = std::get_if<d::PlaylistRename>(&*rows[i].edit)) {
                assert(rename->fromPath != oldName && "never a rename of the old playlist");
            }
        }
    }
    if (del < 0) {
        std::cerr << "FAIL: \"" << oldName << "\" is not offered for deletion\n";
    }
    assert(del >= 0 && included(controller, del) && "the old playlist's delete, ticked");
    assert(sharedConflict && "the group's conflict as before");
    for (const char *section : sections) {
        controller.setSectionIncluded(QString::fromLatin1(section), false);
    }
    controller.setIncluded(del, true);
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    const QVariantMap saved =
        saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
    assert(saved.value("error").toString().isEmpty());
    assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + oldName + "';") == 0);
    const auto after = testing::readBaseline(stick);
    assert(after && !after->findPlaylistByPath(oldName));
    for (const auto &[key, state] : after->declined) {
        assert(d::parseItemKey(key));
    }
    for (const auto &row : rowsOf(controller)) {
        assert(row.header.key != oldKey);
    }
    std::cout << "case 13 (#126: a reused id in a group sharing a path: the old playlist offered and deleted) OK\n";
}

// 14. Issue #125 on a copy of the fixture's stick: the record says id N
//     was "Reused Old" holding Q (three tracks) and P0, the first track of
//     rekordbox's "Playlist 006"; Engine's "Reused Old" holds the same;
//     rekordbox's id N is "Playlist 006", which keeps P0 alone of the
//     four. Neither certainly another playlist nor certainly the same one:
//     a conflict under the id. Answered and saved:
//     a: rekordbox's side: "Reused Old" deleted, "Playlist 006" created
//        with rekordbox's tracks; the delete staged directly before the
//        create, as #99's;
//     b: Engine's side: "Reused Old" renamed "Playlist 006" and given
//        rekordbox's tracks;
//     c: as a, the DJ's Cancel arriving once "Playlist 006" is created;
//     d: as a, the Cancel arriving once "Reused Old" is deleted: the
//        create does not land, and the next analysis offers it, ticked,
//        with its tracks; staged and saved, the stick is a's.
//     Every time (d after its second save): Engine has no "Reused Old"
//     and its "Playlist 006" holds rekordbox's tracks; the record holds
//     "Playlist 006" under the id with exactly rekordbox's tracks in
//     rekordbox's order, never Q; the next analysis lists nothing under
//     the id and no "Reused Old"; and rekordbox putting the first of Q
//     into "Playlist 006" is offered as that add, ticked, alone.
//     e: Engine's "Reused Old" also holds X, put in on the player since
//        the record: both readings would keep X and so write the same,
//        so nothing is asked. One rename row, ticked as any rename, its
//        reason saying why; saved with the default ticks: no delete,
//        "Reused Old" renamed, Q taken out and rekordbox's tracks put in;
//        X stays, after rekordbox's tracks.
//     f: as e, the Cancel arriving once the save's first change of the
//        id has landed: the rest is one unit with it (the playlist's key)
//        and lands too, so X is never left nowhere.
//     For e and f, X is listed as Engine's own afterwards, nothing more.
enum class UnsureSave { Rekordbox, Engine, CancelAfterCreate, CancelAfterDelete, EngineAdded, EngineAddedCancel };

void testUnsureReuse(const fs::path &fixture, UnsureSave how)
{
    namespace d = seabass::domain;
    namespace engine = seabass::infrastructure::engine;
    const char *const names[] = {"export_sync_unsure_rekordbox",      "export_sync_unsure_engine",
                                 "export_sync_unsure_cancel_create",  "export_sync_unsure_cancel_delete",
                                 "export_sync_unsure_engine_added",   "export_sync_unsure_engine_added_cancel"};
    const char *const letters[] = {"a", "b", "c", "d", "e", "f"};
    const auto stick = testing::makeEngineChangeStick(fixture, names[static_cast<int>(how)]);
    const std::string letter = letters[static_cast<int>(how)];
    const bool rekordboxSide = how != UnsureSave::Engine;
    const bool engineAdded = how == UnsureSave::EngineAdded || how == UnsureSave::EngineAddedCancel;
    const std::string newName = "Playlist 006";
    const std::string oldName = "Reused Old";
    const std::string enginePath = pathToUtf8(stick.engine);
    const char *sections[] = {"playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                              "cuesToEngine", "restoresToRekordbox"};

    // A record of the stick as it stands: a save with nothing ticked.
    RekordboxExportSyncController controller;
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    LibraryEditSession *session = sessionOf(stick);
    for (const char *section : sections) {
        controller.setSectionIncluded(QString::fromLatin1(section), false);
    }
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
    auto baseline = testing::readBaseline(stick);
    assert(baseline);
    d::BaselinePlaylist *reused = nullptr;
    for (auto &playlist : baseline->playlists) {
        if (playlist.path == newName) {
            reused = &playlist;
        }
    }
    assert(reused && !reused->folder);

    seabass::infrastructure::rekordbox::KaitaiRekordboxReader rekordboxReader(pathToUtf8(stick.pioneer));
    const std::vector<d::Track> rekordbox = rekordboxReader.readAll();
    engine::LibdjinteropEngineReader engineReader(enginePath);
    const std::vector<d::Track> engineTracks = seabass::application::ScanLibrary(engineReader).execute();
    std::map<std::string, std::int64_t> engineIdByKey;
    for (const auto &[r, e] : d::matchTracks(rekordbox, engineTracks, d::MatchScope::OneStick)) {
        engineIdByKey[testing::keyOf(stick, *r)] = std::stoll(e->sourceId);
    }
    std::vector<std::pair<int, const d::Track *>> placed;
    for (const auto &track : rekordbox) {
        for (const auto &membership : track.playlists) {
            if (membership.name == newName) {
                placed.emplace_back(membership.position, &track);
            }
        }
    }
    std::sort(placed.begin(), placed.end());
    std::vector<std::string> rekordboxKeys;   // its tracks, in its order
    std::vector<std::string> rekordboxFiles;  // their file names
    for (const auto &entry : placed) {
        rekordboxKeys.push_back(testing::keyOf(stick, *entry.second));
        rekordboxFiles.push_back(pathToUtf8(pathFromUtf8(entry.second->filePath).filename()));
    }
    assert(rekordboxKeys.size() >= 3 && "the fixture's playlist has tracks");
    const std::set<std::string> rekordboxSet(rekordboxKeys.begin(), rekordboxKeys.end());
    std::vector<std::string> q;
    for (const auto &entry : engineIdByKey) {
        if (!rekordboxSet.count(entry.first) && q.size() < 3) {
            q.push_back(entry.first);
        }
    }
    assert(q.size() == 3);
    std::vector<std::string> old = q;
    old.push_back(rekordboxKeys.front());  // P0: one of the four kept
    // X (e, f): a track neither list holds, put into Engine's copy on the
    // player since the record.
    std::string xKey;
    for (const auto &entry : engineIdByKey) {
        if (!rekordboxSet.count(entry.first) && std::find(q.begin(), q.end(), entry.first) == q.end()) {
            xKey = entry.first;
            break;
        }
    }
    assert(!xKey.empty());
    std::string xFile;
    for (const auto &track : engineTracks) {
        if (track.sourceId == std::to_string(engineIdByKey.at(xKey))) {
            xFile = pathToUtf8(pathFromUtf8(track.filePath).filename());
        }
    }
    assert(!xFile.empty());

    const std::uint32_t id = reused->id;
    const std::string key = "playlist:" + std::to_string(id);
    const std::string oldKey = "reused:" + std::to_string(id) + ":" + oldName;
    reused->path = oldName;
    reused->members = old;
    for (auto it = baseline->declined.begin(); it != baseline->declined.end();) {
        const bool ofTheId = it->first == key || it->first == "order:" + std::to_string(id)
            || it->first.rfind("member:" + std::to_string(id) + ":", 0) == 0;
        it = ofTheId ? baseline->declined.erase(it) : std::next(it);
    }
    testing::plantBaseline(stick, *baseline);

    // Engine: its "Playlist 006" becomes "Reused Old", holding what the
    // record says.
    assert(testing::sqlExec(stick.db, "UPDATE Playlist SET title = '" + oldName + "' WHERE title = '" + newName
                                          + "' AND parentListId = 0;"));
    for (const std::int64_t track : testing::rootPlaylistTracks(stick.db, oldName)) {
        engine::removeFromEnginePlaylist(enginePath, oldName, track);
    }
    for (const auto &member : old) {
        engine::addToEnginePlaylist(enginePath, oldName, engineIdByKey.at(member));
    }
    assert(testing::rootPlaylistTracks(stick.db, oldName).size() == old.size());
    if (engineAdded) {
        engine::addToEnginePlaylist(enginePath, oldName, engineIdByKey.at(xKey));
    }

    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    int conflict = -1;
    int renameRow = -1;
    for (std::size_t i = 0; i < rowsOf(controller).size(); ++i) {
        const Row &row = rowsOf(controller)[i];
        if (row.section == Section::Conflicts && row.header.key == key) {
            conflict = static_cast<int>(i);
        }
        const auto *rename = row.edit ? std::get_if<d::PlaylistRename>(&*row.edit) : nullptr;
        if (rename && rename->fromPath == oldName && rename->toPath == newName) {
            renameRow = static_cast<int>(i);
        }
    }
    if (engineAdded) {
        // Both readings would keep X: nothing to ask, one rename, ticked as
        // any rename, whose reason says so.
        if (conflict >= 0) {
            std::cerr << "FAIL: asked \"" << rowsOf(controller)[static_cast<std::size_t>(conflict)].header.reasonText
                      << "\"\n";
        }
        assert(conflict < 0 && "no question with one answer");
        assert(renameRow >= 0 && included(controller, renameRow));
        const std::string &why = rowsOf(controller)[static_cast<std::size_t>(renameRow)].header.reasonText;
        std::cout << "  the rename: " << why << "\n";
        assert(why.find("the 1 track added on the player") != std::string::npos
               && why.find("\"" + oldName + "\" is not deleted, and this is not asked") != std::string::npos);
    } else {
        assert(conflict >= 0 && "a conflict under the id");
        controller.resolveConflict(conflict, rekordboxSide);
    }
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());

    // The staging order: rekordbox's side deletes the old playlist directly
    // before it creates the new one, so no stopped save lands the create
    // without the delete (#99's rule).
    // Where Engine's copy holds X (e, f), no delete at all: a stop between
    // it and the adds would leave X nowhere.
    int createAt = -1;
    int deleteAt = -1;
    {
        const auto &pending = session->pendingChanges();
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (pending[i]->unitKeys().contains(QString::fromStdString(key)) && createAt < 0) {
                createAt = static_cast<int>(i);
            }
            if (pending[i]->unitKeys().contains(QString::fromStdString(oldKey))) {
                deleteAt = static_cast<int>(i);
            }
        }
        const bool plain = rekordboxSide && !engineAdded;
        if (plain && (deleteAt < 0 || createAt != deleteAt + 1)) {
            std::cerr << "FAIL: the old playlist's delete is staged at " << deleteAt << " under its own key, the create at "
                      << createAt << "\n";
        }
        if (engineAdded && deleteAt >= 0) {
            std::cerr << "FAIL: Engine's copy holding X is deleted (change " << deleteAt << ")\n";
        }
        assert(createAt >= 0);
        assert(!(engineAdded && deleteAt >= 0) && "no delete of Engine's copy holding X");
        assert((!plain || (deleteAt >= 0 && createAt == deleteAt + 1))
               && "the old playlist's delete directly before the new one's create");
    }

    const auto saveStoppedAfter = [&](int at) {
        const std::vector<std::shared_ptr<PendingChange>> staged = session->pendingChanges();
        QStringList ids;
        for (const auto &change : staged) {
            ids << change->id();
        }
        session->unstageAll(ids);
        std::vector<std::unique_ptr<PendingChange>> restaged;
        for (std::size_t i = 0; i < staged.size(); ++i) {
            restaged.push_back(std::make_unique<Forwarded>(staged[i], static_cast<int>(i) == at));
        }
        assert(session->stageAll(std::move(restaged)));
        const QVariantMap result =
            saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
        std::cout << "  cancelled save: error \"" << result.value("error").toString().toStdString() << "\", warning \""
                  << result.value("warning").toString().toStdString() << "\"\n";
        assert(result.value("cancelled").toBool() && result.value("error").toString().isEmpty());
        assert(result.value("warning").toString().isEmpty());
    };
    if (how == UnsureSave::CancelAfterCreate) {
        saveStoppedAfter(createAt);
    } else if (how == UnsureSave::EngineAddedCancel) {
        // Once the answer's first change has landed, whatever it is.
        saveStoppedAfter(deleteAt >= 0 ? std::min(deleteAt, createAt) : createAt);
    } else if (how == UnsureSave::CancelAfterDelete) {
        saveStoppedAfter(deleteAt);
        // The delete landed, the create did not: nothing under the id.
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + oldName + "';") == 0);
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + newName + "';") == 0);
        const auto record = testing::readBaseline(stick);
        assert(record);
        if (const auto *under = record->findPlaylist(id)) {
            std::cerr << "FAIL: the record holds id " << id << " as \"" << under->path << "\"\n";
            assert(false);
        }
        assert(!record->findPlaylistByPath(oldName));
        bool createOffered = false;
        int adds = 0;
        for (const auto &row : rowsOf(controller)) {
            if (row.header.key != key && row.header.key.rfind("member:" + std::to_string(id) + ":", 0) != 0) {
                continue;
            }
            std::cout << "  then lists: " << row.kind.toStdString() << " " << row.header.key << ": "
                      << row.header.reasonText << "\n";
            assert(row.section != Section::Conflicts && "no conflict");
            assert(row.header.reasonText.find("took this track out") == std::string::npos);
            if (row.kind == QStringLiteral("createPlaylist")) {
                createOffered = row.included;
            } else if (row.kind == QStringLiteral("addMember") && row.included) {
                ++adds;
            }
        }
        assert(createOffered && "the create offered again, ticked");
        assert(adds == static_cast<int>(rekordboxKeys.size()) && "with every track's add");
        controller.stageSelected();
        assert(controller.errorMessage().isEmpty());
        const QVariantMap summary =
            saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
        assert(summary.value("error").toString().isEmpty());
    } else {
        const QVariantMap summary =
            saveAndWait(session, controller, [](LibraryEditSession *s, RekordboxExportSyncController &) { s->save(); });
        assert(summary.value("error").toString().isEmpty());
        assert(summary.value("warning").toString().isEmpty());
    }

    // Engine: no "Reused Old"; "Playlist 006" holds rekordbox's tracks.
    assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + oldName + "';") == 0);
    if (testing::sqlScalar(stick.db, "SELECT count(*) FROM Playlist WHERE title = '" + newName + "';") != 1) {
        std::cerr << "FAIL: Engine has no \"" << newName << "\"" << (engineAdded ? " and X is in no playlist" : "")
                  << "\n";
        assert(false);
    }
    {
        std::vector<std::string> files;
        for (const std::int64_t track : testing::rootPlaylistTracks(stick.db, newName)) {
            const auto path = testing::sqlRows(stick.db, "SELECT path FROM Track WHERE id = " + std::to_string(track) + ";");
            assert(path.size() == 1);
            files.push_back(pathToUtf8(pathFromUtf8(path[0][0]).filename()));
        }
        std::cout << "  \"" << newName << "\" holds " << files.size() << " of rekordbox's " << rekordboxFiles.size()
                  << " tracks\n";
        std::vector<std::string> want = rekordboxFiles;
        if (engineAdded) {
            want.push_back(xFile);  // X kept, after rekordbox's tracks
        }
        if (files != want) {
            std::cerr << "FAIL: \"" << newName << "\" holds " << files.size() << " tracks, not " << want.size()
                      << (engineAdded && std::find(files.begin(), files.end(), xFile) == files.end() ? " (X lost)" : "")
                      << "\n";
        }
        assert(files == want);
    }

    // The record: rekordbox's playlist under the id, exactly its tracks in
    // its order, nothing of "Reused Old".
    {
        const auto record = testing::readBaseline(stick);
        assert(record);
        const auto *under = record->findPlaylist(id);
        assert(under && under->path == newName);
        if (under->members != rekordboxKeys) {
            std::cerr << "FAIL: the record holds " << under->members.size() << " tracks under \"" << newName
                      << "\", rekordbox " << rekordboxKeys.size() << ":\n";
            for (const auto &member : under->members) {
                std::cerr << "    " << member << "\n";
            }
        }
        assert(under->members == rekordboxKeys && "exactly what was applied");
        assert(!record->findPlaylistByPath(oldName));
        for (const auto &[declinedKey, state] : record->declined) {
            assert(d::parseItemKey(declinedKey));
            assert(declinedKey != key && declinedKey != "order:" + std::to_string(id)
                   && declinedKey.rfind("member:" + std::to_string(id) + ":", 0) != 0);
        }
    }

    // The page analysed the stick again after the save: nothing under the
    // id, no "Reused Old"; X (e, f) listed as Engine's own.
    const std::string xItem = "member:" + std::to_string(id) + ":" + xKey;
    const auto xListedAsOwn = [&](const Row &row) {
        return engineAdded && row.header.key == xItem && row.section == Section::EngineOwnKept;
    };
    bool xOwn = false;
    for (const auto &row : rowsOf(controller)) {
        const bool ofTheId = row.header.key == key || row.header.key == oldKey
            || row.header.key.rfind("member:" + std::to_string(id) + ":", 0) == 0
            || row.header.key == "order:" + std::to_string(id);
        if (xListedAsOwn(row)) {
            xOwn = true;
            continue;
        }
        if (ofTheId || row.title.toStdString().find(oldName) != std::string::npos) {
            std::cerr << "FAIL: listed again: " << row.kind.toStdString() << " " << row.header.key << ": "
                      << row.header.reasonText << "\n";
            assert(false);
        }
    }
    assert(xOwn == engineAdded && "X is Engine's own");

    // rekordbox puts the first of Q into "Playlist 006": that add, ticked,
    // and nothing else under the id.
    {
        const auto *q0 = baseline->findTrack(q[0]);
        assert(q0 && q0->pdbId != 0);
        seabass::infrastructure::rekordbox::PdbRowWriter pdb(pathToUtf8(stick.pioneer / "rekordbox" / "export.pdb"));
        assert(pdb.appendPlaylistEntries(id, {q0->pdbId}) == 1);
        assert(pdb.commit());
    }
    analyze(controller, stick);
    assert(controller.errorMessage().isEmpty());
    int add = -1;
    int others = 0;
    for (std::size_t i = 0; i < rowsOf(controller).size(); ++i) {
        const Row &row = rowsOf(controller)[i];
        const bool ofTheId = row.header.key == key || row.header.key == "order:" + std::to_string(id)
            || row.header.key.rfind("member:" + std::to_string(id) + ":", 0) == 0;
        if (!ofTheId || xListedAsOwn(row)) {
            continue;
        }
        std::cout << "  after rekordbox's add: " << row.kind.toStdString() << " " << row.header.key << ": "
                  << row.header.reasonText << "\n";
        const auto *m = row.edit ? std::get_if<d::MembershipEdit>(&*row.edit) : nullptr;
        if (row.header.key == "member:" + std::to_string(id) + ":" + q[0] && m && m->kind == d::MembershipEdit::Kind::Add
            && m->playlistPath == newName && row.header.reasonText.find("took this track out") == std::string::npos) {
            add = static_cast<int>(i);
        } else {
            ++others;
        }
    }
    assert(add >= 0 && included(controller, add) && "the add is offered, ticked");
    assert(others == 0 && "nothing else under the id");
    std::cout << "case 14" << letter << " (#125: a reused id answered "
              << (rekordboxSide ? "rekordbox's way" : "Engine's way")
              << (how == UnsureSave::CancelAfterCreate ? ", cancelled after the create"
                  : how == UnsureSave::CancelAfterDelete ? ", cancelled after the delete"
                  : how == UnsureSave::EngineAdded       ? ", Engine's copy holding X, kept"
                  : how == UnsureSave::EngineAddedCancel ? ", Engine's copy holding X, cancelled after the first change"
                                                         : "")
              << ": the record holds exactly rekordbox's list, a later add offered alone) OK\n";
}

int main(int argc, char **argv)
{
    seabass::tests::assumeNothingElseRunning();
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

    // One case alone, for a quick red/green run (argv[2]: a to f of
    // 14, or "waiting" for 15).
    if (argc >= 3) {
        const std::string which = argv[2];
        if (which == "waiting") {
            testWaitingAnswers();
            return 0;
        }
        const std::map<std::string, UnsureSave> cases{{"a", UnsureSave::Rekordbox},
                                                      {"b", UnsureSave::Engine},
                                                      {"c", UnsureSave::CancelAfterCreate},
                                                      {"d", UnsureSave::CancelAfterDelete},
                                                      {"e", UnsureSave::EngineAdded},
                                                      {"f", UnsureSave::EngineAddedCancel}};
        const auto it = cases.find(which);
        if (it == cases.end()) {
            std::cerr << "unknown case " << which << "\n";
            return 2;
        }
        testUnsureReuse(fixture, it->second);
        return 0;
    }

    testOrderAndDetails();
    testWaitingAnswers();
    testMetadataWords();
    testProposalTicksAndAnswers(fixture);
    testSmallSave(fixture);
    testOrderOnFirstRun(fixture);
    testDeclinedByDependency(fixture);
    testCancelUnits(fixture);
    testEmptyAnswerWithARecord(fixture);
    testReusedPlaylistId(fixture, ReusedSave::Members);
    testReusedPlaylistId(fixture, ReusedSave::Bare);
    testReusedPlaylistId(fixture, ReusedSave::OldLeft);
    testReusedPlaylistId(fixture, ReusedSave::NewLeft);
    testReusedPlaylistId(fixture, ReusedSave::Declined);
    testReusedPlaylistId(fixture, ReusedSave::RenameDeclined);
    testReusedPlaylistId(fixture, ReusedSave::MemberDeclined);
    testReusedPlaylistId(fixture, ReusedSave::CancelAfterDelete);
    testReusedPlaylistId(fixture, ReusedSave::CancelBeforeBoth);
    testReusedPlaylistId(fixture, ReusedSave::CancelAfterDeleteMemberUnticked);
    testReusedPlaylistId(fixture, ReusedSave::CancelBeforeBothMemberUnticked);
    testReusedPlaylistId(fixture, ReusedSave::CancelBeforeBothCreateUnticked);
    testReusedAtASharedPath(fixture);
    testUnsureReuse(fixture, UnsureSave::Rekordbox);
    testUnsureReuse(fixture, UnsureSave::Engine);
    testUnsureReuse(fixture, UnsureSave::CancelAfterCreate);
    testUnsureReuse(fixture, UnsureSave::CancelAfterDelete);
    testUnsureReuse(fixture, UnsureSave::EngineAdded);
    testUnsureReuse(fixture, UnsureSave::EngineAddedCancel);
    std::cout << "rekordbox_export_sync_controller_test: all cases OK\n";
    return 0;
}
