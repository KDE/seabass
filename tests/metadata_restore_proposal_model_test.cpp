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

    // ---- case 2: a landed change finds its row, and removing a row keeps
    //      every other row's staging with its own proposal
    {
        RestoreProposalListModel model;
        model.setProposals({first, copy, other});
        const QString copyChange = QStringLiteral("metadata-restore:engine:102");
        const QString otherChange = QStringLiteral("metadata-restore:rekordbox:103");
        model.setStagedChanges(1, {copyChange});
        model.setStagedChanges(2, {otherChange});
        assert(model.indexOfChange(copyChange) == 1 && model.indexOfChange(otherChange) == 2);
        assert(model.indexOfChange(QStringLiteral("metadata-restore:nothing:1")) == -1);
        model.removeAt(0);
        assert(model.isStaged(0) && model.isStaged(1) && model.stagedCount() == 2);
        assert(model.indexOfChange(otherChange) == 1);
        std::cout << "case 2 (staging stays with its proposal when a row goes) OK\n";
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
        model.removeAt(1);
        assert(!summaryAtRow(model, 0).contains(QStringLiteral("other track")));
        std::cout << "case 4 (each row says what a restore writes, where, and to how many) OK\n";
    }

    std::cout << "metadata_restore_proposal_model_test: all cases passed\n";
    return 0;
}
