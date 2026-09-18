// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/mark_rekordbox_imported_change.hpp"

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_import_state.hpp"

#include <string>

namespace seabass::gui
{

namespace
{
constexpr const char *Format = "engine";
constexpr const char *Label = "engine-import-counter";

std::string databaseOf(const QString &enginePath)
{
    return FormatWriteSession::databaseFileFor(Format, enginePath.toStdString());
}
}  // namespace

MarkRekordboxImportedChange::MarkRekordboxImportedChange(QString enginePath, std::uint64_t librarySequence)
    : m_enginePath(std::move(enginePath)), m_librarySequence(librarySequence)
{
}

QString MarkRekordboxImportedChange::idFor()
{
    return QStringLiteral("engine-import-counter");
}

QString MarkRekordboxImportedChange::id() const
{
    return idFor();
}

QString MarkRekordboxImportedChange::owner() const
{
    return QStringLiteral("library-health");
}

QString MarkRekordboxImportedChange::description() const
{
    return QStringLiteral("Tell Engine this stick's rekordbox library is already imported, so the player stops "
                          "offering to overwrite the Engine side with it");
}

QString MarkRekordboxImportedChange::unit() const
{
    return QStringLiteral("libraries");
}

QString MarkRekordboxImportedChange::verb() const
{
    return QStringLiteral("marked");
}

QStringList MarkRekordboxImportedChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> MarkRekordboxImportedChange::filesToBackup(SaveContext &) const
{
    return {{databaseOf(m_enginePath), Label}};
}

ChangeOutcome MarkRekordboxImportedChange::apply(SaveContext &ctx)
{
    FormatWriteSession &session = sharedFormatWriteSession(ctx, Format, m_enginePath.toStdString(), 1, Label);
    const std::string database = FormatWriteSession::databaseFileFor(Format, session.writeRoot());

    std::string error;
    const bool ok = infrastructure::engine::markRekordboxLibraryImported(
        m_enginePath.toStdString(), m_librarySequence, &error,
        [&ctx](const std::string &file) { ctx.protectForThisChange(file); }, database);
    if (!ok) {
        return ChangeOutcome::failure(QString::fromStdString(error));
    }
    session.noteItemApplied();
    ctx.log().record("engine import counter: set to " + std::to_string(m_librarySequence)
                     + ", so the player no longer offers to re-import the rekordbox library");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
