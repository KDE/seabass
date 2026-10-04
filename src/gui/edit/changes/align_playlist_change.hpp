// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/playlist_sync.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Makes one playlist hold the same tracks in every library on the stick
// as it does in the library the user chose (domain::alignTo): rows added
// to and taken out of the others' copies of that playlist, each library
// by its own writer. export.pdb only ever loses an entry here: adding a
// row to it is #62, so the controller leaves additions to rekordbox, and
// the OneLibrary additions that would part it from rekordbox, out of what
// it stages.
class AlignPlaylistChange : public PendingChange
{
public:
    AlignPlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, std::string reference,
                        std::vector<domain::PlaylistAlignment> alignments, int itemCountHint);

    static QString idFor(const std::string &playlist);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    std::string m_reference;
    std::vector<domain::PlaylistAlignment> m_alignments;
    int m_itemCountHint = 1;
};

}  // namespace seabass::gui
