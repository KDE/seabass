// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/level_cue_table_change.hpp"

#include <utility>

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

LevelCueTableChange::LevelCueTableChange(QString pioneerRoot, std::vector<Row> rows)
    : m_pioneerRoot(std::move(pioneerRoot)), m_rows(std::move(rows))
{
}

QString LevelCueTableChange::idFor()
{
    return QStringLiteral("library-health:level-cue-tables");
}

QString LevelCueTableChange::id() const
{
    return idFor();
}

QString LevelCueTableChange::owner() const
{
    return QStringLiteral("library-health");
}

QString LevelCueTableChange::description() const
{
    return m_rows.size() == 1
        ? QStringLiteral("Remove the cues only the OneLibrary cue table of \"%1\" holds").arg(subject())
        : QStringLiteral("Remove the cues only the OneLibrary cue table holds, for %1 tracks").arg(m_rows.size());
}

QString LevelCueTableChange::subject() const
{
    if (m_rows.size() != 1) {
        return QStringLiteral("OneLibrary cue tables");
    }
    return QString::fromStdString(m_rows[0].title.empty() ? m_rows[0].filePath : m_rows[0].title);
}

QString LevelCueTableChange::unit() const
{
    return QStringLiteral("tracks");
}

QString LevelCueTableChange::verb() const
{
    return QStringLiteral("repaired");
}

int LevelCueTableChange::unitsWritten() const
{
    return m_repaired;
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
    m_repaired = 0;
    try {
        ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
        auto &writer = sharedOneLibraryWriter(ctx, pioneer);
        std::vector<std::pair<int64_t, int64_t>> doomed;  // (content_id, cue_id)
        for (const Row &r : m_rows) {
            const std::string row = "content_id " + std::to_string(r.contentId) + " (" + r.filePath + ")";
            const auto file = writer.analysisFileCuesOf(r.contentId);
            if (!file) {
                ctx.log().record(std::string(Label) + ": " + row + " has no analysis file to read, its cue table left alone");
                continue;
            }
            const auto excess = domain::entriesNotInFile(writer.cueTableOf(r.contentId), *file,
                                                         domain::cueToleranceMsFor(r.bpm, r.bpm));
            if (excess.empty()) {
                ctx.log().record(std::string(Label) + ": " + row + " holds no cue its analysis file does not, left alone");
                continue;
            }
            for (const auto &e : excess) {
                doomed.emplace_back(r.contentId, e.cueId);
            }
            ++m_repaired;
            ctx.log().record(std::string(Label) + ": " + row + " loses the cues only its table held: "
                             + domain::describeCuePlaces(domain::cuesOf(excess)));
        }
        if (doomed.empty()) {
            return ChangeOutcome::skip();
        }
        writer.removeCueRows(doomed);
    } catch (const std::exception &e) {
        m_repaired = 0;
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
