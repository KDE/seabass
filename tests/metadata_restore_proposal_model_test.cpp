// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The restore page's proposal list. Staging is kept per proposal, not per
// stored track: one stored track can match several tracks on the stick, and
// keyed by the stored id, Select All staged only the first of them while
// Select None cleared the second's entry and found nothing left for the
// first -- about every second row stayed ticked on a stick with duplicated
// files. Each row also says what a restore writes, where, and to how many.

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "domain/metadata_restore.hpp"
#include "gui/metadata_restore_proposal_model.hpp"

using seabass::domain::MetadataRestoreProposal;
using seabass::gui::RestoreProposalListModel;

namespace
{

int nextRowId = 100;

MetadataRestoreProposal proposal(const std::string &storedId, const std::string &file,
                                 const std::vector<std::string> &formats)
{
    MetadataRestoreProposal p;
    p.storedId = storedId;
    p.stickTrack.title = file;
    p.stickTrack.filename = file;
    p.stickTrack.filePath = "/stick/Contents/" + file;
    for (const auto &format : formats) {
        seabass::domain::CatalogRowRef row;
        row.format = format;
        row.sourceId = std::to_string(nextRowId++);
        p.stickTrack.catalogRows.push_back(row);
    }
    return p;
}

bool stagedAtRow(const RestoreProposalListModel &model, int row)
{
    return model.data(model.index(row), RestoreProposalListModel::StagedRole).toBool();
}

QString summaryAtRow(const RestoreProposalListModel &model, int row)
{
    return model.data(model.index(row), RestoreProposalListModel::RestoreSummaryRole).toString();
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // Two tracks on the stick matched to one stored track, and one more.
    MetadataRestoreProposal first = proposal("7", "Major Tom.mp3", {"rekordbox", "onelibrary"});
    first.cuesOffered = true;
    first.cuesConflict = true;
    first.cues.resize(3);
    first.stickTrack.cues.resize(2);
    first.ratingOffered = true;
    first.rating = 4;
    MetadataRestoreProposal copy = proposal("7", "Major Tom (copy).mp3", {"engine"});
    copy.commentOffered = true;
    copy.comment = "drop at 1:04";
    MetadataRestoreProposal other = proposal("9", "Other.mp3", {"rekordbox"});
    other.commentOffered = true;
    other.comment = "rekordbox alone cannot take this";

    // ---- case 1: two tracks matched to one stored track stage apart
    {
        RestoreProposalListModel model;
        model.setProposals({first, copy, other});
        model.setStagedChanges(0, {QStringLiteral("metadata-restore:rekordbox:100")});
        model.setStagedChanges(1, {QStringLiteral("metadata-restore:engine:102")});
        assert(model.isStaged(0) && model.isStaged(1) && model.stagedCount() == 2);
        assert(stagedAtRow(model, 0) && stagedAtRow(model, 1));

        // Unstaging one leaves the other exactly as it was -- the second
        // half of the every-second-row bug.
        model.setStagedChanges(1, {});
        assert(model.isStaged(0) && !model.isStaged(1) && model.stagedCount() == 1);
        assert(stagedAtRow(model, 0) && !stagedAtRow(model, 1));
        model.setStagedChanges(0, {});
        assert(model.stagedCount() == 0 && !stagedAtRow(model, 0));
        std::cout << "case 1 (two tracks matched to one stored track stage and unstage apart) OK\n";
    }

    // ---- case 2: removing rows keeps every other row's staging with its
    //      own proposal, and the counts with it
    {
        RestoreProposalListModel model;
        model.setProposals({first, copy, other});
        const QString firstChange = QStringLiteral("metadata-restore:rekordbox:100");
        const QString copyChange = QStringLiteral("metadata-restore:engine:102");
        const QString otherChange = QStringLiteral("metadata-restore:rekordbox:103");
        model.setStagedChanges(0, {firstChange});
        model.setStagedChanges(1, {copyChange});
        model.setStagedChanges(2, {otherChange});
        assert(model.stagedCount() == 3 && model.stagedChangeCount() == 3);
        // Out of order, repeated and out of range: taken once each, the
        // rest ignored.
        model.removeAll({2, 0, 2, -1, 7});
        assert(model.totalCount() == 1 && model.rowCount() == 1);
        assert(model.stagedChanges(0) == QStringList{copyChange} && "the survivor keeps its own staging");
        assert(model.stagedCount() == 1 && model.stagedChangeCount() == 1);
        model.removeAll({});
        assert(model.totalCount() == 1);
        std::cout << "case 2 (staging stays with its proposal when rows go) OK\n";
    }

    // ---- case 3: a search narrows the view, not the staging
    {
        RestoreProposalListModel model;
        model.setProposals({first, copy, other});
        model.setStagedChanges(2, {QStringLiteral("metadata-restore:rekordbox:103")});
        model.setFilter(QStringLiteral("other"));
        assert(model.rowCount() == 1 && stagedAtRow(model, 0));
        model.setFilter(QString());
        assert(model.rowCount() == 3 && stagedAtRow(model, 2) && !stagedAtRow(model, 0));
        std::cout << "case 3 (a search narrows the view, not the staging) OK\n";
    }

    // ---- case 4: each row says what a restore writes, where, and to how many
    {
        RestoreProposalListModel model;
        model.setProposals({first, copy, other});
        const QString firstSays = summaryAtRow(model, 0);
        assert(firstSays.contains(QStringLiteral("3 cues, replacing the 2 the track has now")));
        assert(firstSays.contains(QStringLiteral("its 4-star rating")));
        assert(firstSays.contains(QStringLiteral("in DeviceLibrary and OneLibrary")));
        assert(firstSays.contains(QStringLiteral("1 other track on this stick")));
        const QString copySays = summaryAtRow(model, 1);
        assert(copySays.contains(QStringLiteral("its comment")) && copySays.contains(QStringLiteral("in Engine")));
        assert(copySays.contains(QStringLiteral("1 other track on this stick")));
        const QString otherSays = summaryAtRow(model, 2);
        assert(!otherSays.contains(QStringLiteral("other track")));
        assert(otherSays.contains(QStringLiteral("comment stays behind")));
        // A comment for DeviceLibrary and OneLibrary says it reaches only OneLibrary;
        // zero stars is spoken as such; a track no catalog lists offers nothing.
        MetadataRestoreProposal both = proposal("11", "Both.mp3", {"rekordbox", "onelibrary"});
        both.commentOffered = true;
        both.comment = "for OneLibrary only";
        both.ratingOffered = true;
        both.rating = 0;
        MetadataRestoreProposal nowhere = proposal("12", "Streaming.mp3", {});
        nowhere.ratingOffered = true;
        nowhere.rating = 3;
        RestoreProposalListModel more;
        more.setProposals({both, nowhere});
        const QString bothSays = summaryAtRow(more, 0);
        assert(bothSays.contains(QStringLiteral("its zero-star rating")));
        assert(bothSays.contains(QStringLiteral("comment goes only to OneLibrary")));
        assert(summaryAtRow(more, 1).startsWith(QStringLiteral("Nothing can be restored")));
        // And once a copy has landed and gone, the other no longer claims one.
        model.removeAll({1});
        assert(!summaryAtRow(model, 0).contains(QStringLiteral("other track")));
        std::cout << "case 4 (each row says what a restore writes, where, and to how many) OK\n";
    }

    // ---- case 5: the scope bounds what Select All stages, not only the view
    //
    // The stick and playlist pickers say what a restore is for, so what
    // they leave out is neither shown nor staged by Select All, and a
    // staged row they leave out is handed back to be unstaged. The search
    // narrows the view within the scope and nothing more.
    {
        MetadataRestoreProposal fromA1 = proposal("20", "A1.mp3", {"rekordbox"});
        fromA1.storedFromLibraryId = "stick-a";
        fromA1.storedPlaylists = {"Warm Up"};
        MetadataRestoreProposal fromA2 = proposal("21", "A2.mp3", {"engine"});
        fromA2.storedFromLibraryId = "stick-a";
        fromA2.stickTrack.playlists = {seabass::domain::PlaylistMembership{"Closing", 1}};
        MetadataRestoreProposal fromB = proposal("22", "B1.mp3", {"rekordbox"});
        fromB.storedFromLibraryId = "stick-b";
        fromB.storedPlaylists = {"Warm Up"};
        for (auto *p : {&fromA1, &fromA2, &fromB}) {
            p->cuesOffered = true;
            p->cues.resize(2);
        }

        RestoreProposalListModel model;
        model.setProposals({fromA1, fromA2, fromB});
        model.setStagedChanges(2, {QStringLiteral("metadata-restore:rekordbox:b")});
        assert(model.scopedCount() == 3 && model.unstagedInScope() == (std::vector<int>{0, 1}));

        model.setScope({"id:stick-a", {}});
        assert(model.rowCount() == 2 && model.scopedCount() == 2);
        assert(model.unstagedInScope() == (std::vector<int>{0, 1}) && "Select All stages stick A's two");
        assert(model.stagedOutsideScope() == (std::vector<int>{2}) && "and B's staged row has to come off");
        assert(!model.inScope(2));

        // One playlist inside that stick. "Warm Up" is on A1 by the
        // backup's record; A2 is only in "Closing", on the stick's own.
        model.setScope({"id:stick-a", "Warm Up"});
        assert(model.rowCount() == 1 && model.scopedCount() == 1);
        assert(model.unstagedInScope() == (std::vector<int>{0}));
        model.setScope({"id:stick-a", "Closing"});
        assert(model.unstagedInScope() == (std::vector<int>{1}) && "the stick's playlists count too");

        // The search narrows the view inside the scope, never the scope:
        // Select All still stages what the search hides.
        model.setScope({{}, "Warm Up"});
        model.setFilter(QStringLiteral("B1"));
        assert(model.rowCount() == 1 && model.scopedCount() == 2);
        assert(model.unstagedInScope() == (std::vector<int>{0}) && "A1 is in scope although the search hides it");
        assert(model.stagedOutsideScope().empty() && "B1 is staged and in scope");
        assert(model.data(model.index(0), RestoreProposalListModel::TitleRole).toString() == QStringLiteral("B1.mp3"));
        std::cout << "case 5 (the scope bounds what Select All stages, the search only what is shown) OK\n";
    }

    // ---- case 6: a row hands the waveform what it needs, and only that
    {
        MetadataRestoreProposal offered = proposal("30", "Offered.mp3", {"rekordbox"});
        offered.stickTrack.durationSeconds = 200.0;
        offered.cuesOffered = true;
        seabass::domain::CuePoint hot;
        hot.kind = seabass::domain::CuePoint::Kind::Hot;
        hot.hotCueNumber = 3;
        hot.positionMs = 64'000.0;
        offered.cues = {hot};
        offered.stickTrack.cues = {};
        offered.storedPlaylists = {"Warm Up"};
        offered.stickTrack.playlists = {seabass::domain::PlaylistMembership{"Closing", 2}};
        MetadataRestoreProposal notOffered = proposal("31", "Kept.mp3", {"rekordbox"});
        notOffered.ratingOffered = true;
        notOffered.rating = 3;
        seabass::domain::CuePoint memory;
        memory.positionMs = 1'000.0;
        notOffered.stickTrack.cues = {memory, memory};

        RestoreProposalListModel model;
        model.setProposals({offered, notOffered});
        const QVariantList cues = model.data(model.index(0), RestoreProposalListModel::CuesRole).toList();
        assert(cues.size() == 1 && "the cues the restore would write");
        assert(cues[0].toMap()[QStringLiteral("positionMs")].toDouble() == 64'000.0);
        assert(cues[0].toMap()[QStringLiteral("kind")].toString() == QStringLiteral("hot"));
        assert(cues[0].toMap()[QStringLiteral("hotCueNumber")].toInt() == 3);
        // The same map every metadata list hands out, the store's own
        // browse list included, and that one always carried the position
        // as text too.
        assert(cues[0].toMap()[QStringLiteral("positionText")].toString() == QStringLiteral("1:04"));
        assert(model.data(model.index(0), RestoreProposalListModel::DurationMsRole).toDouble() == 200'000.0);
        assert(model.data(model.index(1), RestoreProposalListModel::CuesRole).toList().size() == 2
               && "cues not on offer: the track keeps its own, so those are what it shows");
        assert(model.data(model.index(0), RestoreProposalListModel::PlaylistNamesRole).toString()
               == QStringLiteral("Warm Up, Closing"));
        std::cout << "case 6 (a row hands the waveform the cues the restore would leave) OK\n";
    }

    std::cout << "metadata_restore_proposal_model_test: all cases passed\n";
    return 0;
}
