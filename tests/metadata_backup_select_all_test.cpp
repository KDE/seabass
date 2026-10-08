// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include <QGuiApplication>

#include "gui/metadata_backup_proposal_model.hpp"

using seabass::domain::MetadataBackupProposal;
using seabass::gui::BackupProposalListModel;

namespace
{
MetadataBackupProposal proposal(const std::string &title, std::vector<std::string> playlists)
{
    MetadataBackupProposal p;
    p.stickTrack.title = title;
    p.stickTrack.sourceId = title;
    for (auto &name : playlists) {
        p.stickTrack.playlists.push_back({name, 1});
    }
    p.isNew = true;
    return p;
}
}  // namespace

// Select All in Metadata Backup stays inside the picked playlist, as the
// Restore page's does: it staged the whole stick while the list showed
// one playlist. The search does not narrow it, and nothing already
// staged outside the playlist is dropped.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    BackupProposalListModel model;
    model.setProposals({proposal("Aria", {"Warmup"}), proposal("Bloom", {"Warmup", "Peak"}),
                        proposal("Closer", {"Peak"})});

    model.setFilter(QString(), QStringLiteral("Warmup"));
    assert(model.rowCount() == 2);
    model.stageAll();
    assert(model.stagedCount() == 2 && "the playlist's two, not the stick's three");
    assert(model.isStaged(0) && model.isStaged(1) && !model.isStaged(2));
    std::cout << "case (Select All stays inside the picked playlist) OK\n";

    model.unstageAll();
    model.setFilter(QStringLiteral("Aria"), QStringLiteral("Warmup"));
    assert(model.rowCount() == 1);
    model.stageAll();
    assert(model.stagedCount() == 2 && "a search does not narrow it: Bloom too");
    std::cout << "case (the search does not narrow Select All) OK\n";

    model.unstageAll();
    model.setStaged(2, true);
    model.setFilter(QString(), QStringLiteral("Warmup"));
    model.stageAll();
    assert(model.stagedCount() == 3 && model.isStaged(2) && "what was staged outside stays");
    std::cout << "case (staging outside the playlist is kept) OK\n";

    model.unstageAll();
    model.setFilter(QString(), QString());
    model.stageAll();
    assert(model.stagedCount() == 3 && "no playlist picked: every track");
    std::cout << "case (no playlist: the whole stick) OK\n";
    return 0;
}
