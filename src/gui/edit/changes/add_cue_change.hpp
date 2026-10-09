// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One cue added to one track. A hot slot can hold exactly one cue, so
// staging the same slot twice replaces the earlier staging rather than
// queueing a second write to the same pad.
class AddCueChange : public PendingChange
{
public:
    AddCueChange(QString format, QString path, QString sourceId, double positionMs, QString kind, int hotCueNumber,
                 QString color, QString comment, bool isLoop, double loopEndMs, QString trackTitle);

    QString id() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;

    // What the page shows for this staged cue, so it can draw the marker
    // before the save runs.
    QVariantMap summary() const;

    ChangeOutcome apply(SaveContext &ctx) override;

    // The track's whole cue set as the last apply() wrote it, on a
    // rekordbox or OneLibrary track: the set is read at apply() time, so
    // only then is it known.
    std::vector<RekordboxWrite> rekordboxWrites() const override;

private:
    std::optional<RekordboxWrite> m_written;
    QString m_format;
    QString m_path;
    QString m_sourceId;
    double m_positionMs;
    QString m_kind;
    int m_hotCueNumber;
    QString m_color;
    QString m_comment;
    bool m_isLoop;
    double m_loopEndMs;
    QString m_trackTitle;
};

}  // namespace seabass::gui
