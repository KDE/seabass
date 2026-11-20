// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One authored field of a track, as Browse's track details edit it (#105).
// Rating and comment today. Another field (title, artist, genre) is one
// more value here, its words in fieldName()/describe, and its write per
// catalog in TrackFieldEditChange::apply(), next to these two: the
// staging, the lookup of the track's rows in each catalog, the backups,
// the rollback and Undo are the same for every field.
enum class TrackField
{
    Rating,
    Comment,
};

struct TrackFieldValue
{
    TrackField field = TrackField::Rating;
    // Rating: 1 to 5 stars, or nullopt for none.
    std::optional<int> rating;
    // Comment: the text, UTF-8, empty for none.
    std::string text;
};

// The change's id for `field` of the file at `filePath`: one per field and
// track, so staging the same field again replaces the earlier value.
QString trackFieldChangeId(TrackField field, const std::string &filePath);

// The value of one field written into every catalog on the stick that
// holds the track, so the three stay level: export.pdb (its rating byte,
// its comment string in place, PdbRowWriter), OneLibrary (content.rating
// and content.djComment, OneLibraryCueWriter) and Engine (Track.rating
// and Track.comment through libdjinterop). The track is found in each by
// its file, the way the moved-file relink finds it (rekordboxLookupSpellings
// for export.pdb, the writers' own lookups for the others), so a
// re-export that renumbered the rows does not matter. A catalog that does
// not list the file is left alone; one that lists it and cannot take the
// value fails the change, and the save puts back everything it wrote.
class TrackFieldEditChange : public PendingChange
{
public:
    TrackFieldEditChange(QString pioneerRoot, QString enginePath, std::string filePath, std::string title,
                         TrackFieldValue value);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList unitKeys() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    std::vector<RekordboxWrite> rekordboxWrites() const override;
    ChangeOutcome apply(SaveContext &ctx) override;

    const TrackFieldValue &value() const { return m_value; }
    const std::string &filePath() const { return m_filePath; }

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_filePath;
    std::string m_title;
    TrackFieldValue m_value;
    // The last apply() wrote the rating into export.pdb or OneLibrary.
    bool m_wroteRekordboxSide = false;
};

// The longest comment every catalog holding the file can take (#105).
// OneLibrary's djComment and Engine's Track.comment are SQLite TEXT
// columns with no length of their own, so the limit comes from export.pdb
// alone: the span its row's comment string takes (it cannot grow), in
// plain ASCII characters or in UTF-16 code units as that field is
// encoded. No limit when export.pdb is absent or does not list the file.
struct CommentLimit
{
    bool limited = false;
    // How many units fit, when limited.
    std::size_t units = 0;
    // The field takes plain ASCII only (units are then characters too).
    bool asciiOnly = false;
    // Why there is a limit, one plain sentence for a tooltip; empty when
    // there is none.
    std::string reason;
};

// Reads export.pdb whole: off the GUI thread.
CommentLimit commentLimitFor(const QString &pioneerRoot, const std::string &filePath);

// Why `comment` cannot be written under `limit`, or empty when it can.
std::string commentLimitRefusal(const std::string &comment, const CommentLimit &limit);

}  // namespace seabass::gui
