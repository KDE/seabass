// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/fill_sample_rate_change.hpp"

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"

#include <string>

namespace seabass::gui
{

namespace
{
constexpr const char *Format = "engine";
constexpr const char *Label = "sample-rate-fill";

std::string databaseOf(const QString &enginePath)
{
    return FormatWriteSession::databaseFileFor(Format, enginePath.toStdString());
}
}  // namespace

FillSampleRateChange::FillSampleRateChange(QString enginePath, infrastructure::engine::SampleRateEntry entry,
                                           int itemCountHint, bool declaresDatabase)
    : m_enginePath(std::move(enginePath))
    , m_entry(std::move(entry))
    , m_itemCountHint(itemCountHint)
    , m_declaresDatabase(declaresDatabase)
{
}

QString FillSampleRateChange::idFor(std::int64_t trackId)
{
    return QStringLiteral("sample-rate:engine:%1").arg(trackId);
}

QString FillSampleRateChange::id() const
{
    return idFor(m_entry.trackId);
}

QString FillSampleRateChange::owner() const
{
    return QStringLiteral("library-health");
}

QString FillSampleRateChange::description() const
{
    const QString title = QString::fromStdString(m_entry.title);
    return QStringLiteral("Give %1 the sample rate its own file reports, %2 Hz")
        .arg(title.isEmpty() ? QStringLiteral("track %1").arg(m_entry.trackId) : QStringLiteral("\"%1\"").arg(title))
        .arg(static_cast<int>(m_entry.sampleRateFromFile));
}

QString FillSampleRateChange::unit() const
{
    return QStringLiteral("tracks");
}

QString FillSampleRateChange::verb() const
{
    return QStringLiteral("filled in");
}

QStringList FillSampleRateChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> FillSampleRateChange::filesToBackup(SaveContext &) const
{
    // Declared once for the batch, by its first change: see
    // RepairArtworkChange::filesToBackup() for why a per-change
    // checkpoint of m.db is the wrong price to pay a thousand times.
    if (!m_declaresDatabase) {
        return {};
    }
    return {{databaseOf(m_enginePath), Label}};
}

ChangeOutcome FillSampleRateChange::apply(SaveContext &ctx)
{
    FormatWriteSession &session =
        sharedFormatWriteSession(ctx, Format, m_enginePath.toStdString(), m_itemCountHint, Label);
    const std::string database = FormatWriteSession::databaseFileFor(Format, session.writeRoot());

    const auto repair = infrastructure::engine::repairSampleRates(
        m_enginePath.toStdString(), {m_entry}, [&ctx](const std::string &file) { ctx.protectForThisChange(file); },
        database);
    if (!repair.error.empty()) {
        return ChangeOutcome::failure(QString::fromStdString(repair.error));
    }
    if (repair.repaired == 0) {
        // The row went, or its file stopped answering between the scan
        // and the save. A skip, not a failure: one track is not worth
        // abandoning the rest of the save for.
        ctx.log().record("sample rates: skipped track " + std::to_string(m_entry.trackId)
                         + ": nothing left to write");
        return ChangeOutcome::success();
    }
    session.noteItemApplied();
    ctx.log().record("sample rates: gave track " + std::to_string(m_entry.trackId) + " its own rate, "
                     + std::to_string(static_cast<int>(m_entry.sampleRateFromFile)) + " Hz");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
