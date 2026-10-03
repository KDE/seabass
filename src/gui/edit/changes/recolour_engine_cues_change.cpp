// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/recolour_engine_cues_change.hpp"

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"

namespace seabass::gui
{

RecolourEngineCuesChange::RecolourEngineCuesChange(QString enginePath, domain::HiddenEngineCues hidden,
                                                   int itemCountHint)
    : m_enginePath(std::move(enginePath)), m_hidden(std::move(hidden)), m_itemCountHint(itemCountHint)
{
}

QString RecolourEngineCuesChange::idFor(const std::string &engineSourceId)
{
    return QStringLiteral("recolour:engine:") + QString::fromStdString(engineSourceId);
}

QString RecolourEngineCuesChange::id() const
{
    return idFor(m_hidden.track.sourceId);
}

QString RecolourEngineCuesChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RecolourEngineCuesChange::description() const
{
    const int n = m_hidden.hidden();
    return QStringLiteral("Give %1 %2 on \"%3\" a colour the player shows")
        .arg(n)
        .arg(n == 1 ? QStringLiteral("cue") : QStringLiteral("cues"))
        .arg(QString::fromStdString(m_hidden.track.title));
}

QString RecolourEngineCuesChange::subject() const
{
    const QString title = QString::fromStdString(m_hidden.track.title.empty() ? m_hidden.track.filename
                                                                               : m_hidden.track.title);
    return m_hidden.track.artist.empty() ? title
                                         : title + QStringLiteral(", ") + QString::fromStdString(m_hidden.track.artist);
}

QString RecolourEngineCuesChange::unit() const
{
    return QStringLiteral("tracks");
}

QString RecolourEngineCuesChange::verb() const
{
    return QStringLiteral("recoloured");
}

QStringList RecolourEngineCuesChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> RecolourEngineCuesChange::filesToBackup(SaveContext &ctx) const
{
    if (m_enginePath.isEmpty()) {
        return {};
    }
    std::vector<BackupTarget> targets;
    const WriteScope scope{.cueData = true, .catalogRows = false, .oneLibraryMirror = false};
    for (const auto &file : filesWrittenFor(scope, {"engine", m_hidden.track.sourceId}, m_enginePath, ctx)) {
        targets.push_back({file, "recolour"});
    }
    return targets;
}

ChangeOutcome RecolourEngineCuesChange::apply(SaveContext &ctx)
{
    if (m_enginePath.isEmpty()) {
        return ChangeOutcome::failure("No Engine catalog path is known for this stick.");
    }
    // The same cues, written through the writer that gives a colourless
    // pad Engine's default for its number. The track's cue point and its
    // coloured pads go back exactly as they were.
    FormatWriteSession &session =
        sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, "recolour");
    infrastructure::engine::LibdjinteropEngineCueWriter writer(session.writeRoot());
    writer.writeHotCues(m_hidden.track.sourceId, m_hidden.track.cues);
    session.noteItemApplied();
    ctx.log().record("recolour: gave " + std::to_string(m_hidden.hidden()) + " hidden cue(s) on engine track id="
                     + m_hidden.track.sourceId + " (\"" + m_hidden.track.title + "\") the pad's default colour");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
