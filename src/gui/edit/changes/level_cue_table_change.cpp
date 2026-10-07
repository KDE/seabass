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
    return static_cast<int>(m_rows.size());
}

int LevelCueTableChange::unitsSkipped() const
{
    return m_skipped;
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
    m_skipped = 0;
    try {
        ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
        auto &writer = sharedOneLibraryWriter(ctx, pioneer);
        std::vector<std::pair<int64_t, int64_t>> doomed;  // (content_id, cue_id)
        std::vector<std::string> removals;                 // logged once they are on the stick
        for (const Row &r : m_rows) {
            const std::string row = "content_id " + std::to_string(r.contentId) + " (" + r.filePath + ")";
            // The scan's rule, row by row, on the stick as it is now.
            const auto check = domain::checkCueTable(writer.cueTableOf(r.contentId), writer.analysisFileCuesOf(r.contentId),
                                                     domain::cueToleranceMsFor(r.bpm, r.bpm));
            if (check.verdict != domain::CueTableVerdict::Excess) {
                ++m_skipped;
                const char *why = check.verdict == domain::CueTableVerdict::NoFileRead ? "its analysis file could not be read"
                    : check.verdict == domain::CueTableVerdict::NotUnderstood ? "its table holds a cue kind Seabass does not know"
                                                                              : "its table holds no cue its analysis file does not";
                ctx.log().record(std::string(Label) + ": " + row + " left alone: " + why);
                continue;
            }
            for (const auto &e : check.notInFile) {
                doomed.emplace_back(r.contentId, e.cueId);
            }
            removals.push_back(std::string(Label) + ": " + row + " lost the cues only its table held: "
                               + domain::describeCuePlaces(domain::cuesOf(check.notInFile)));
        }
        if (doomed.empty()) {
            return ChangeOutcome::skip();
        }
        writer.removeCueRows(doomed);
        for (const auto &line : removals) {
            ctx.log().record(line);
        }
    } catch (const std::exception &e) {
        m_skipped = 0;
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
