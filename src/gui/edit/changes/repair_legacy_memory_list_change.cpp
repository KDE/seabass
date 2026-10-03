// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/repair_legacy_memory_list_change.hpp"

#include "application/path_key.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

#include <set>

namespace seabass::gui
{

RepairLegacyMemoryListChange::RepairLegacyMemoryListChange(domain::Track track,
                                                           infrastructure::rekordbox::LegacyMemoryListFinding finding,
                                                           infrastructure::rekordbox::KeepCueList keep)
    : m_track(std::move(track)), m_finding(std::move(finding)), m_keep(keep)
{
}

QString RepairLegacyMemoryListChange::idFor(const std::string &analyzePath)
{
    return QStringLiteral("legacy-memory-list:") + QString::fromStdString(application::normalizedPathKey(analyzePath));
}

QString RepairLegacyMemoryListChange::id() const
{
    return idFor(m_finding.analyzePath);
}

QString RepairLegacyMemoryListChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RepairLegacyMemoryListChange::description() const
{
    const QString title = QString::fromStdString(m_track.title);
    QStringList parts;
    if (m_finding.shape.repairable()) {
        parts << QStringLiteral("rebuild the memory cue list of \"%1\"").arg(title);
    }
    if (m_finding.listsFixable()) {
        parts << (m_keep == infrastructure::rekordbox::KeepCueList::Player
                      ? QStringLiteral("give \"%1\" the cues the player shows in both cue lists")
                      : QStringLiteral("give \"%1\" the cues Seabass wrote in both cue lists"))
                     .arg(title);
    }
    if (!m_finding.debris.empty()) {
        parts << QStringLiteral("remove the analysis file a player left beside \"%1\"").arg(title);
    }
    QString text = parts.join(QStringLiteral(", then "));
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
    }
    return text;
}

QString RepairLegacyMemoryListChange::subject() const
{
    const QString title = QString::fromStdString(m_track.title.empty() ? m_track.filename : m_track.title);
    return m_track.artist.empty() ? title : title + QStringLiteral(", ") + QString::fromStdString(m_track.artist);
}

QString RepairLegacyMemoryListChange::unit() const
{
    return QStringLiteral("tracks");
}

QString RepairLegacyMemoryListChange::verb() const
{
    return QStringLiteral("repaired");
}

QStringList RepairLegacyMemoryListChange::formatsTouched() const
{
    // OneLibrary reads its rows' cues from these same analysis files
    // (#59), so its cached cues go stale with rekordbox's.
    if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(m_finding.pioneerRoot)) {
        return {QStringLiteral("rekordbox"), QStringLiteral("onelibrary")};
    }
    return {QStringLiteral("rekordbox")};
}

std::vector<BackupTarget> RepairLegacyMemoryListChange::filesToBackup(SaveContext &) const
{
    // The .DAT the list sits in, and every file the repair removes: Undo
    // puts the debris back too, so nothing a player wrote is gone for
    // good on the strength of this check alone. export.pdb is not
    // touched and not named.
    // When the lists are rewritten, RekordboxCueWriter may write both
    // files of the pair (rekordboxCueFilesFor names them).
    std::vector<BackupTarget> targets;
    if (m_finding.listsFixable()) {
        for (const auto &file : infrastructure::rekordbox::rekordboxCueFilesFor(m_finding.pioneerRoot,
                                                                                 m_finding.analyzePath)) {
            targets.push_back({file, "legacy-memory-list"});
        }
    } else if (m_finding.shape.repairable()) {
        targets.push_back({m_finding.datPath, "legacy-memory-list"});
    }
    for (const auto &debris : m_finding.debris) {
        targets.push_back({debris, "legacy-memory-list"});
    }
    // exportLibrary.db is not named: whether its cue table is refreshed
    // is known only once apply() has asked OneLibrary which analysis
    // files the track's rows name, and naming it here when it is not
    // written would have Undo put back a file the save never touched.
    // apply() backs it up (backupOnce) just before writing it.
    return targets;
}

bool RepairLegacyMemoryListChange::refreshesOneLibraryTable() const
{
    return m_finding.listsFixable() && !m_track.filePath.empty()
           && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(m_finding.pioneerRoot);
}

ChangeOutcome RepairLegacyMemoryListChange::apply(SaveContext &ctx)
{
    const std::string account = infrastructure::rekordbox::repairTrackAnalysis(m_finding, m_keep);
    ctx.log().record("legacy-memory-list: " + m_track.format + " track id=" + m_track.sourceId + " (\""
                     + m_track.title + "\"): " + account);
    // Where the lists were rewritten, OneLibrary's cue table for this
    // file is refreshed from what the file now holds, through the same
    // secondary write every OneLibrary cue write makes (#59): no player
    // measured reads the table, but rekordbox desktop may on import, and
    // it must not hold cues the file no longer has. The file itself is
    // left alone by that write, since it already holds these cues. A file
    // OneLibrary does not list has no table rows to refresh.
    //
    // writeCuesForPath() writes every analysis file the audio file's
    // OneLibrary rows name. When one of them is another file (a second
    // analysis only OneLibrary lists), this file's cues must not go
    // there, so the table is then left as it is and the log says why.
    if (refreshesOneLibraryTable()) {
        try {
            auto &mirror = sharedOneLibraryWriter(ctx, m_finding.pioneerRoot);
            if (!mirror.hasTrackAtPath(m_track.filePath)) {
                ctx.log().record("legacy-memory-list: OneLibrary does not list this file; no cue table to refresh");
                return ChangeOutcome::success();
            }
            const std::set<std::string> own{
                application::normalizedPathKey(
                    infrastructure::rekordbox::datAnlzPath(m_finding.pioneerRoot, m_finding.analyzePath)),
                application::normalizedPathKey(
                    infrastructure::rekordbox::extAnlzPath(m_finding.pioneerRoot, m_finding.analyzePath))};
            for (const auto &file : mirror.cueFilesForPath(m_track.filePath)) {
                if (!own.count(application::normalizedPathKey(file))) {
                    ctx.log().record("legacy-memory-list: OneLibrary names " + file
                                     + " for this audio file as well; its cue table is left as it is");
                    return ChangeOutcome::success();
                }
            }
        } catch (const std::exception &e) {
            return ChangeOutcome::failure(
                QStringLiteral("Could not read Device Library Plus to refresh its cue table: %1. The save stops here "
                               "and puts back what this change wrote.")
                    .arg(QString::fromUtf8(e.what())));
        }
        infrastructure::rekordbox::FilesystemAnlzSource source(m_finding.pioneerRoot);
        const auto cues = infrastructure::rekordbox::readAnalysisFileCues(source, m_finding.analyzePath);
        if (cues) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(m_finding.pioneerRoot),
                           "legacy-memory-list");
            const QString why = mirrorCuesOrExplain(sharedOneLibraryWriter(ctx, m_finding.pioneerRoot),
                                                    m_track.filePath, *cues, ctx, "legacy-memory-list",
                                                    QStringLiteral("refresh the cue table"));
            if (!why.isEmpty()) {
                return ChangeOutcome::failure(why);
            }
        }
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
