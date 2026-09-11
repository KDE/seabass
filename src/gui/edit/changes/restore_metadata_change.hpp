// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/metadata_restore.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Puts one track's stored cues back on the stick, in one catalog.
//
// One change per (track, catalog), the same shape MergeCuesChange uses,
// because each catalog is a different file with a different writer and a
// different thing to back up. The rekordbox branch additionally mirrors
// into OneLibrary, so a track that both formats list needs no separate
// OneLibrary change -- see MetadataRestoreController, which is where
// that decision is made.
//
// The proposal carries the complete cue list to end up with, never a
// diff: every cue writer in Seabass replaces a track's whole set.
class RestoreMetadataChange : public PendingChange
{
public:
    // itemCountHint: how many tracks this save is expected to restore.
    // It only drives the scratch-copy decision (see FormatWriteSession),
    // and the session takes it from whichever change applies first, so it
    // is a lower bound rather than a total. Getting it wrong costs speed,
    // never correctness.
    RestoreMetadataChange(QString format, QString path, QString sourceId,
                           domain::MetadataRestoreProposal proposal, int itemCountHint = 1);

    QString id() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_format;
    QString m_path;
    QString m_sourceId;
    domain::MetadataRestoreProposal m_proposal;
    int m_itemCountHint = 1;
};

}  // namespace seabass::gui
