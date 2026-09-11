// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/delete_orphan_change.hpp"

#include <memory>
#include <string>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

namespace seabass::gui
{

DeleteOrphanChange::DeleteOrphanChange(QString path, domain::LibraryConsistencyIssue issue)
    : m_path(std::move(path)), m_issue(std::move(issue))
{
}

QString DeleteOrphanChange::id() const
{
    return "orphan:" + issueKeyFor(m_issue);
}

QString DeleteOrphanChange::owner() const
{
    return QStringLiteral("library-health");
}

QString DeleteOrphanChange::description() const
{
    QString title =
        m_issue.brokenGroup.empty() ? QString("?") : QString::fromStdString(m_issue.brokenGroup.front().title);
    return QStringLiteral("Delete %1 orphaned OneLibrary row(s) (\"%2\")").arg(m_issue.brokenGroup.size()).arg(title);
}

QString DeleteOrphanChange::verb() const
{
    return QStringLiteral("removed");
}

QString DeleteOrphanChange::unit() const
{
    // "entries", not "rows": a row is what the catalog calls it, an
    // entry is what a person sees in a list. The summary is read by
    // someone who just deleted something and wants to know what.
    return QStringLiteral("entries");
}

QStringList DeleteOrphanChange::formatsTouched() const
{
    return {"onelibrary"};
}

// OneLibrary only, one database, path derived from the root alone.
std::vector<BackupTarget> DeleteOrphanChange::filesToBackup(SaveContext &ctx) const
{
    std::vector<BackupTarget> targets;
    // The row id is irrelevant for OneLibrary -- one shared database
    // whatever the track -- but the format is what selects that branch.
    for (const auto &file : filesWrittenFor(WriteScope{}, {"onelibrary", std::string()}, m_path, ctx)) {
        targets.push_back({file, "consistency-delete-orphan"});
    }
    return targets;
}

ChangeOutcome DeleteOrphanChange::apply(SaveContext &ctx)
{
    std::string root = m_path.toStdString();
    struct Writer
    {
        explicit Writer(const std::string &pioneerRoot) : writer(pioneerRoot) {}
        infrastructure::onelibrary::OneLibraryCueWriter writer;
    };
    Writer &w = ctx.shared<Writer>("orphan:onelibrary", [&]() {
        ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), "consistency-delete-orphan");
        return std::make_unique<Writer>(root);
    });
    for (const auto &broken : m_issue.brokenGroup) {
        w.writer.removeTrackByPath(broken.filePath);
        ctx.log().record("consistency: deleted orphaned OneLibrary row \"" + broken.title + "\"");
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
