// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/finish_cleanup_change.hpp"

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

#include <stdexcept>

namespace seabass::gui
{

namespace
{
constexpr const char *Label = "finish-cleanup";
}

FinishCleanupChange::FinishCleanupChange(QString pioneerRoot, domain::CleanupLeftover leftover, bool declaresDatabase)
    : m_pioneerRoot(std::move(pioneerRoot)), m_leftover(std::move(leftover)), m_declaresDatabase(declaresDatabase)
{
    if (m_leftover.kind != domain::CleanupLeftover::Kind::Repairable || !m_leftover.survivor) {
        // The finder decides what is repairable; staging anything else
        // would drop playlist entries with nowhere to go.
        throw std::invalid_argument("FinishCleanupChange: not a repairable leftover");
    }
}

QString FinishCleanupChange::idFor(const std::string &leftoverFilePath)
{
    return QStringLiteral("finish-cleanup:onelibrary:") + QString::fromStdString(leftoverFilePath);
}

QString FinishCleanupChange::id() const
{
    return idFor(m_leftover.row.filePath);
}

QString FinishCleanupChange::owner() const
{
    return QStringLiteral("library-health");
}

QString FinishCleanupChange::description() const
{
    const QString title = QString::fromStdString(m_leftover.row.title);
    return QStringLiteral("Remove the copy of %1 Clean Up left in OneLibrary")
        .arg(title.isEmpty() ? QStringLiteral("a track") : QStringLiteral("\"%1\"").arg(title));
}

QString FinishCleanupChange::unit() const
{
    // What the DJ sees listed twice is a track; the rows behind it are
    // the catalog's business (see DeleteOrphanChange::unit()).
    return QStringLiteral("duplicates");
}

QString FinishCleanupChange::verb() const
{
    return QStringLiteral("removed");
}

QStringList FinishCleanupChange::formatsTouched() const
{
    return {QStringLiteral("onelibrary")};
}

std::vector<BackupTarget> FinishCleanupChange::filesToBackup(SaveContext &) const
{
    if (!m_declaresDatabase) {
        return {};
    }
    return {{infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(m_pioneerRoot.toStdString()), Label}};
}

ChangeOutcome FinishCleanupChange::apply(SaveContext &ctx)
{
    const std::string root = m_pioneerRoot.toStdString();
    // Every change, not only the first: the first may have been unstaged,
    // and backupOnce() costs nothing after the first call of a save.
    ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), Label);
    auto &writer = sharedOneLibraryWriter(ctx, root);
    const std::string &doomed = m_leftover.row.filePath;
    const std::string &survivor = m_leftover.survivor->filePath;
    try {
        writer.removeTrackByPathReplacingWith(doomed, survivor);
    } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
        // Gone since the scan (a second Seabass window, or a save of this
        // page's own that already took it). Nothing left to finish.
        ctx.log().record("finish-cleanup: skipped \"" + m_leftover.row.title + "\": no longer in OneLibrary");
        return ChangeOutcome::skip();
    } catch (const infrastructure::onelibrary::OneLibrarySurvivorMissing &) {
        // The copy the entries were to move onto went since the scan.
        // Removing the leftover anyway would drop its playlist entries,
        // which is the one thing this repair exists not to do.
        ctx.log().record("finish-cleanup: skipped \"" + m_leftover.row.title + "\": the copy Clean Up kept, "
                         + survivor + ", is no longer in OneLibrary");
        return ChangeOutcome::skip();
    }
    ctx.log().record("finish-cleanup: removed \"" + m_leftover.row.title + "\" (" + doomed
                     + ") from OneLibrary, its playlist entries moved to " + survivor);
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
