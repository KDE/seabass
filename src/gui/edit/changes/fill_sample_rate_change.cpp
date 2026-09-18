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

FillSampleRateChange::FillSampleRateChange(QString enginePath,
                                           std::vector<infrastructure::engine::SampleRateEntry> entries)
    : m_enginePath(std::move(enginePath)), m_entries(std::move(entries))
{
}

QString FillSampleRateChange::idFor()
{
    return QStringLiteral("sample-rates:engine");
}

QString FillSampleRateChange::id() const
{
    return idFor();
}

QString FillSampleRateChange::owner() const
{
    return QStringLiteral("library-health");
}

QString FillSampleRateChange::description() const
{
    return QStringLiteral("Fill in the sample rate for %1 track(s), read from the files themselves")
        .arg(m_entries.size());
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
    return {{databaseOf(m_enginePath), Label}};
}

ChangeOutcome FillSampleRateChange::apply(SaveContext &ctx)
{
    FormatWriteSession &session = sharedFormatWriteSession(ctx, Format, m_enginePath.toStdString(),
                                                            static_cast<int>(m_entries.size()), Label);
    const std::string database = FormatWriteSession::databaseFileFor(Format, session.writeRoot());

    const auto repair = infrastructure::engine::repairSampleRates(
        m_enginePath.toStdString(), m_entries, [&ctx](const std::string &file) { ctx.protectForThisChange(file); },
        database);
    if (!repair.error.empty()) {
        return ChangeOutcome::failure(QString::fromStdString(repair.error));
    }
    for (int i = 0; i < repair.repaired; ++i) {
        session.noteItemApplied();
    }
    ctx.log().record("sample rates: filled in " + std::to_string(repair.repaired) + " track(s), skipped "
                     + std::to_string(repair.skipped)
                     + " whose file could not say after all");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
