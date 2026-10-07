// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/level_cue_table_change.hpp"

#include "domain/cue_tolerance.hpp"
#include "domain/onelibrary_cue_table.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

namespace seabass::gui
{

namespace
{

constexpr const char *Label = "level-onelibrary-cue-table";

}  // namespace

LevelCueTableChange::LevelCueTableChange(QString pioneerRoot, Row row)
    : m_pioneerRoot(std::move(pioneerRoot)), m_row(std::move(row))
{
}

QString LevelCueTableChange::idFor(int64_t contentId)
{
    return QStringLiteral("library-health:level-cue-table:%1").arg(contentId);
}

QString LevelCueTableChange::id() const
{
    return idFor(m_row.contentId);
}

QString LevelCueTableChange::owner() const
{
    return QStringLiteral("library-health");
}

QString LevelCueTableChange::description() const
{
    return QStringLiteral("Match the OneLibrary cue table of \"%1\" to its analysis file").arg(subject());
}

QString LevelCueTableChange::subject() const
{
    return QString::fromStdString(m_row.title.empty() ? m_row.filePath : m_row.title);
}

QString LevelCueTableChange::unit() const
{
    return QStringLiteral("tracks");
}

QString LevelCueTableChange::verb() const
{
    return QStringLiteral("repaired");
}

QStringList LevelCueTableChange::formatsTouched() const
{
    return {QStringLiteral("onelibrary")};
}

std::vector<BackupTarget> LevelCueTableChange::filesToBackup(SaveContext &) const
{
    return {{infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(m_pioneerRoot.toStdString()), Label}};
}

ChangeOutcome LevelCueTableChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string row = "content_id " + std::to_string(m_row.contentId) + " (" + m_row.filePath + ")";
    try {
        ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
        auto &writer = sharedOneLibraryWriter(ctx, pioneer);
        const auto file = writer.analysisFileCuesOf(m_row.contentId);
        if (!file) {
            ctx.log().record(std::string(Label) + ": " + row + " has no analysis file to read, its cue table left alone");
            return ChangeOutcome::skip();
        }
        const auto table = writer.cueTableOf(m_row.contentId);
        if (domain::cuesNotInFile(table, *file, domain::cueToleranceMsFor(m_row.bpm, m_row.bpm)).empty()) {
            ctx.log().record(std::string(Label) + ": " + row + " holds no cue its analysis file does not, left alone");
            return ChangeOutcome::skip();
        }
        writer.writeCueTableOf(m_row.contentId, *file);
        ctx.log().record(std::string(Label) + ": " + row + " cue table set to the " + std::to_string(file->size())
                         + " cue(s) of its analysis file");
    } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
        ctx.log().record(std::string(Label) + ": " + row + " is gone, nothing to repair");
        return ChangeOutcome::skip();
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
