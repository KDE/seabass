// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/repair_artwork_change.hpp"

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/artwork_rescue_sources.hpp"

#include <string>

namespace seabass::gui
{

namespace
{
constexpr const char *Format = "engine";
constexpr const char *Label = "artwork-repair";

// The library's own database, under the path the page already holds (the
// "Engine Library" folder itself, not the stick root). Through
// FormatWriteSession, so this is the same spelling the save's redirect is
// keyed on rather than a fourth hand-built copy of the path.
std::string databaseOf(const QString &enginePath)
{
    return FormatWriteSession::databaseFileFor(Format, enginePath.toStdString());
}
}  // namespace

RepairArtworkChange::RepairArtworkChange(QString enginePath, infrastructure::engine::ArtworkEntry entry,
                                         int itemCountHint, bool declaresDatabase,
                                         std::shared_ptr<ArtworkRescueSources> rescue)
    : m_enginePath(std::move(enginePath))
    , m_entry(std::move(entry))
    , m_itemCountHint(itemCountHint)
    , m_declaresDatabase(declaresDatabase)
    , m_rescue(std::move(rescue))
{
}

QString RepairArtworkChange::idFor(std::int64_t trackId)
{
    return QStringLiteral("artwork:engine:%1").arg(trackId);
}

QString RepairArtworkChange::id() const
{
    return idFor(m_entry.trackId);
}

QString RepairArtworkChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RepairArtworkChange::description() const
{
    const QString title = QString::fromStdString(m_entry.title);
    return QStringLiteral("Give %1 cover art a player can find")
        .arg(title.isEmpty() ? QStringLiteral("track %1").arg(m_entry.trackId) : QStringLiteral("\"%1\"").arg(title));
}

QString RepairArtworkChange::unit() const
{
    return QStringLiteral("tracks");
}

QString RepairArtworkChange::verb() const
{
    return QStringLiteral("repaired");
}

QStringList RepairArtworkChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> RepairArtworkChange::filesToBackup(SaveContext &) const
{
    // Declared once for the batch, by its first change.
    //
    // This list feeds two things. backupAllNow() makes the user-facing
    // backup, and folds every request under one label into one record, so
    // naming the database a thousand times adds nothing there. But
    // beginChange() also takes a whole-file checkpoint copy of everything
    // declared, per change, and endChange() throws it away again -- so a
    // thousand tracks meant a thousand sequential copies of m.db through
    // the temporary directory, which is RAM-backed here. On the stick this
    // was written for that is gigabytes of copying to protect a file that
    // does not need it.
    //
    // It does not need it because each track's rows are written inside
    // their own transaction and rolled back on any error (see
    // repairArtwork), so there is no half-written database for a
    // checkpoint to put back. What a failed change does need taken out is
    // the image file it copied, and that is protected per change through
    // beforeWrite -> protectForThisChange, not through this list.
    if (!m_declaresDatabase) {
        return {};
    }
    return {{databaseOf(m_enginePath), Label}};
}

ChangeOutcome RepairArtworkChange::apply(SaveContext &ctx)
{
    // The one write session for this database and save: the first track
    // decides whether the run goes through a scratch copy, and the rest
    // write the same one. Its commit puts the copy back on the stick.
    FormatWriteSession &session =
        sharedFormatWriteSession(ctx, Format, m_enginePath.toStdString(), m_itemCountHint, Label);
    const std::string database = FormatWriteSession::databaseFileFor(Format, session.writeRoot());

    // Each image is copied into the library before its row is written, so
    // a save that fails afterwards takes the files back out with the rest.
    // The images are not redirected: they are new files under Artwork/, on
    // the stick, not the database the scratch copy stands in for.
    const auto repair = infrastructure::engine::repairArtwork(
        m_enginePath.toStdString(), {m_entry}, [&ctx](const std::string &file) { ctx.protectForThisChange(file); },
        database, m_rescue ? m_rescue->reader() : infrastructure::engine::ArtworkSourceReader{});
    if (!repair.error.empty()) {
        return ChangeOutcome::failure(QString::fromStdString(repair.error));
    }
    if (repair.repaired == 0) {
        // This one track could not be given art. That is a skip, not a
        // failure: a failed change stops the whole save (save_loop.cpp
        // breaks on the first one), so failing here would mean one
        // unreadable cover among a thousand abandoned the other nine
        // hundred and ninety-nine -- while repairArtwork's own comment
        // says a cover is not worth failing a save for. A real write or
        // database error is a different thing and came back above, as an
        // error string.
        //
        // All three are rare by construction: the audit only offers a
        // track whose image is on this stick and reads as a JPEG or a PNG,
        // so each of these means something changed underneath the page
        // since it was scanned -- and the page rescans after the save, so
        // the track shows up as still unrepaired rather than vanishing
        // from the report.
        std::string why;
        if (repair.tracksNoLongerThere > 0) {
            why = "the track is no longer in the library";
        } else if (repair.notAnImage > 0) {
            why = m_entry.imageOnStick + " is not a JPEG or a PNG";
        } else {
            why = m_entry.imageOnStick + " could not be read";
        }
        ctx.log().record("artwork: skipped track " + std::to_string(m_entry.trackId) + ": " + why);
        return ChangeOutcome::success();
    }
    session.noteItemApplied();
    ctx.log().record("artwork: gave track " + std::to_string(m_entry.trackId)
                     + " cover art Engine can find, " + std::to_string(repair.filesWritten.size())
                     + " image(s) copied into the library");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
