// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/track_field_edit_change.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "application/path_key.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/moved_file_changes.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

constexpr const char *Label = "track-edit";

std::string exportPdbOf(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

std::string engineDbOf(const std::string &enginePath)
{
    return pathToUtf8(pathFromUtf8(enginePath) / "Database2" / "m.db");
}

bool isFile(const std::string &path)
{
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(pathFromUtf8(path), ec);
}

QString fieldName(TrackField field)
{
    return field == TrackField::Rating ? QStringLiteral("rating") : QStringLiteral("comment");
}

// export.pdb's rows for the file: its own spelling first, then the disk's
// (rekordboxLookupSpellings, #103).
std::vector<uint32_t> pdbRowsFor(const infrastructure::rekordbox::PdbRowWriter &pdb, const std::string &pioneerRoot,
                                 const std::string &filePath)
{
    for (const std::string &onStick : rekordboxLookupSpellings(pioneerRoot, filePath)) {
        auto ids = pdb.trackIdsWithFilePath(onStick);
        if (!ids.empty()) {
            return ids;
        }
    }
    return {};
}

}  // namespace

QString trackFieldChangeId(TrackField field, const std::string &filePath)
{
    return QStringLiteral("addcue:track-") + fieldName(field) + QLatin1Char(':')
           + QString::fromStdString(application::normalizedPathKey(filePath));
}

TrackFieldEditChange::TrackFieldEditChange(QString pioneerRoot, QString enginePath, std::string filePath,
                                           std::string title, TrackFieldValue value)
    : m_pioneerRoot(std::move(pioneerRoot)),
      m_enginePath(std::move(enginePath)),
      m_filePath(std::move(filePath)),
      m_title(std::move(title)),
      m_value(std::move(value))
{
}

QString TrackFieldEditChange::id() const
{
    return trackFieldChangeId(m_value.field, m_filePath);
}

QString TrackFieldEditChange::owner() const
{
    return QStringLiteral("addcue");
}

QString TrackFieldEditChange::subject() const
{
    return QString::fromStdString(m_title.empty() ? pathToUtf8(pathFromUtf8(m_filePath).filename()) : m_title);
}

QString TrackFieldEditChange::description() const
{
    if (m_value.field == TrackField::Rating) {
        if (!m_value.rating) {
            return QStringLiteral("Clear the rating of \"%1\"").arg(subject());
        }
        return QStringLiteral("Rate \"%1\" %2 star%3")
            .arg(subject())
            .arg(*m_value.rating)
            .arg(*m_value.rating == 1 ? QString() : QStringLiteral("s"));
    }
    if (m_value.text.empty()) {
        return QStringLiteral("Clear the comment on \"%1\"").arg(subject());
    }
    return QStringLiteral("Change the comment on \"%1\"").arg(subject());
}

QString TrackFieldEditChange::unit() const
{
    return QStringLiteral("tracks");
}

QString TrackFieldEditChange::verb() const
{
    return QStringLiteral("edited");
}

// A track's rating and its comment are one edit of the track: a cancel
// keeps them together.
QStringList TrackFieldEditChange::unitKeys() const
{
    return {QStringLiteral("track-edit:") + QString::fromStdString(application::normalizedPathKey(m_filePath))};
}

QStringList TrackFieldEditChange::formatsTouched() const
{
    QStringList formats;
    const std::string pioneer = m_pioneerRoot.toStdString();
    if (isFile(pioneer.empty() ? std::string() : exportPdbOf(pioneer))) {
        formats << QStringLiteral("rekordbox");
    }
    if (!pioneer.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
        formats << QStringLiteral("onelibrary");
    }
    if (isFile(m_enginePath.isEmpty() ? std::string() : engineDbOf(m_enginePath.toStdString()))) {
        formats << QStringLiteral("engine");
    }
    return formats;
}

// Every catalog the stick has, before anything is written: which of them
// list the file is only known at apply(), and a backup of one the save
// then leaves alone puts back what is already there.
std::vector<BackupTarget> TrackFieldEditChange::filesToBackup(SaveContext &ctx) const
{
    (void)ctx;
    std::vector<BackupTarget> targets;
    const std::string pioneer = m_pioneerRoot.toStdString();
    if (!pioneer.empty() && isFile(exportPdbOf(pioneer))) {
        targets.push_back({exportPdbOf(pioneer), Label});
    }
    if (!pioneer.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
        targets.push_back({infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label});
    }
    if (!m_enginePath.isEmpty() && isFile(engineDbOf(m_enginePath.toStdString()))) {
        targets.push_back({engineDbOf(m_enginePath.toStdString()), Label});
    }
    return targets;
}

// The rating, for the rekordbox baseline's origin ledger: Sync after
// Rekordbox Export then knows Seabass wrote it. 0 is "no stars". A
// comment has no place in that record (RekordboxWrite carries none).
std::vector<RekordboxWrite> TrackFieldEditChange::rekordboxWrites() const
{
    if (m_value.field != TrackField::Rating || !m_wroteRekordboxSide) {
        return {};
    }
    RekordboxWrite write;
    write.filePath = m_filePath;
    write.rating = m_value.rating.value_or(0);
    return {std::move(write)};
}

ChangeOutcome TrackFieldEditChange::apply(SaveContext &ctx)
{
    m_wroteRekordboxSide = false;
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string engine = m_enginePath.toStdString();
    const bool rating = m_value.field == TrackField::Rating;
    if (rating && m_value.rating && (*m_value.rating < 1 || *m_value.rating > 5)) {
        return ChangeOutcome::failure(QStringLiteral("A rating is 1 to 5 stars."));
    }
    QStringList written;
    try {
        if (!pioneer.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            if (writer.hasTrackAtPath(m_filePath)) {
                if (rating) {
                    writer.writeAnnotationForPath(m_filePath, m_value.rating.value_or(0), std::nullopt);
                } else {
                    writer.writeAnnotationForPath(m_filePath, std::nullopt, m_value.text);
                }
                m_wroteRekordboxSide = true;
                written << QStringLiteral("OneLibrary");
            }
        }
        if (!engine.empty() && isFile(engineDbOf(engine))) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "engine", engine, 1, Label);
            const auto ids =
                infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(), m_filePath);
            if (!ids.empty()) {
                infrastructure::engine::LibdjinteropEngineCueWriter writer(session.writeRoot());
                for (const auto id : ids) {
                    if (rating) {
                        writer.writeRating(std::to_string(id), m_value.rating);
                    } else {
                        writer.writeAnnotation(std::to_string(id), std::nullopt, m_value.text);
                    }
                }
                session.noteItemApplied();
                written << QStringLiteral("Engine");
            }
        }
        // export.pdb last: its write is a whole-file replace, the one
        // most likely to meet a stick that is full or gone, and a failure
        // there puts back the two databases written before it.
        if (!pioneer.empty() && isFile(exportPdbOf(pioneer))) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, 1, Label);
            infrastructure::rekordbox::PdbRowWriter pdb(exportPdbOf(session.writeRoot()));
            const auto ids = pdbRowsFor(pdb, pioneer, m_filePath);
            for (const uint32_t id : ids) {
                if (rating) {
                    // 0 is how export.pdb says "no stars": the byte has no
                    // other value for it.
                    pdb.setTrackRating(id, m_value.rating.value_or(0));
                } else {
                    std::string refusal;
                    if (!pdb.setTrackComment(id, m_value.text, &refusal)) {
                        return ChangeOutcome::failure(
                            QStringLiteral("The comment on \"%1\" cannot be written into export.pdb: %2.")
                                .arg(subject(), QString::fromStdString(refusal.empty() ? "its row has gone" : refusal)));
                    }
                }
            }
            if (!ids.empty()) {
                if (!pdb.commit()) {
                    return ChangeOutcome::failure(QStringLiteral("Could not write export.pdb."));
                }
                session.noteItemApplied();
                m_wroteRekordboxSide = true;
                written << QStringLiteral("export.pdb");
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QStringLiteral("Could not write the %1 of \"%2\": %3")
                                          .arg(fieldName(m_value.field), subject(), QString::fromUtf8(e.what())));
    }
    if (written.isEmpty()) {
        ctx.log().record(std::string(Label) + ": no catalog lists " + m_filePath + " any more; nothing written");
        return ChangeOutcome::skip();
    }
    ctx.log().record(std::string(Label) + ": " + description().toStdString() + " in "
                     + written.join(QStringLiteral(", ")).toStdString());
    return ChangeOutcome::success();
}

CommentLimit commentLimitFor(const QString &pioneerRoot, const std::string &filePath)
{
    CommentLimit limit;
    const std::string pioneer = pioneerRoot.toStdString();
    if (pioneer.empty() || !isFile(exportPdbOf(pioneer))) {
        return limit;
    }
    try {
        const infrastructure::rekordbox::PdbRowWriter pdb(exportPdbOf(pioneer));
        for (const uint32_t id : pdbRowsFor(pdb, pioneer, filePath)) {
            const auto capacity = pdb.trackCommentCapacity(id);
            const std::size_t units = capacity ? capacity->units : 0;
            limit.units = limit.limited ? std::min(limit.units, units) : units;
            limit.asciiOnly = limit.asciiOnly || !capacity || !capacity->utf16;
            limit.limited = true;
        }
    } catch (const std::exception &e) {
        limit.limited = true;
        limit.units = 0;
        limit.reason = std::string("export.pdb could not be read, so the comment cannot be changed: ") + e.what();
        return limit;
    }
    if (limit.limited) {
        limit.reason = limit.units == 0
            ? "export.pdb keeps no room for a comment on this track, and Seabass cannot make its row larger."
            : "export.pdb keeps room for " + std::to_string(limit.units) + " characters in this track's comment"
                  + (limit.asciiOnly ? ", plain ASCII only (no accented letters)," : "")
                  + " and Seabass cannot make its row larger.";
    }
    return limit;
}

std::string commentLimitRefusal(const std::string &comment, const CommentLimit &limit)
{
    if (!limit.limited) {
        return {};
    }
    return infrastructure::rekordbox::PdbRowWriter::trackCommentRefusal(
        comment, infrastructure::rekordbox::PdbRowWriter::TrackCommentCapacity{limit.units, !limit.asciiOnly});
}

}  // namespace seabass::gui
