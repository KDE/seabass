// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/copy_cues_change.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "application/ports/cue_writer.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/onelibrary_cue_writer_adapter.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

// One format's cue writer plus the rule for which files a given track's
// write needs backed up first. rekordbox names a different file per
// track (its analysis file); the database formats name one file for all.
// Writers only. Which files a write touches is answered by
// filesWrittenFor() in change_helpers -- one definition, so a change cannot
// back up one file while overwriting another.
struct DuplicatesFormatContext
{
    std::unique_ptr<application::CueWriter> writer;
};

// oneLibrarySourceIdToPath is only consulted when format == "onelibrary"
// -- built by the caller from the actual tracks about to be written
// (OneLibraryCueWriterAdapter's own comment explains why sourceId alone
// isn't enough for this format).
std::unique_ptr<DuplicatesFormatContext> makeContext(
    const QString &format, const QString &path,
    const std::unordered_map<std::string, std::string> &oneLibrarySourceIdToPath)
{
    auto ctx = std::make_unique<DuplicatesFormatContext>();
    if (format == "rekordbox") {
        std::string pioneerRoot = path.toStdString();
        ctx->writer = std::make_unique<infrastructure::rekordbox::RekordboxCueWriter>(pioneerRoot);
    } else if (format == "onelibrary") {
        std::string pioneerRoot = path.toStdString();  // same PIONEER root rekordbox uses, see scan()'s own comment
        ctx->writer = std::make_unique<OneLibraryCueWriterAdapter>(pioneerRoot, oneLibrarySourceIdToPath);
    } else {
        std::string engineLibraryPath = path.toStdString();
        ctx->writer = std::make_unique<infrastructure::engine::LibdjinteropEngineCueWriter>(engineLibraryPath);
    }
    return ctx;
}

}  // namespace

CopyCuesChange::CopyCuesChange(QString format, QString path, QString groupKey, DuplicatesCopyOp op)
    : m_format(std::move(format)), m_path(std::move(path)), m_groupKey(std::move(groupKey)), m_op(std::move(op))
{
}

QString CopyCuesChange::id() const
{
    return "dup:" + m_format + ":" + m_groupKey;
}

QString CopyCuesChange::description() const
{
    QString from = QString::fromStdString(m_op.source.filename.empty() ? m_op.source.sourceId : m_op.source.filename);
    QString title = QString::fromStdString(m_op.source.title);
    return QStringLiteral("Copy %1 cue(s) from %2 onto %3 other copy/copies of \"%4\"")
        .arg(m_op.source.cues.size())
        .arg(from)
        .arg(m_op.targets.size())
        .arg(title);
}

QString CopyCuesChange::verb() const
{
    return QStringLiteral("copied");
}

QString CopyCuesChange::unit() const
{
    return QStringLiteral("groups");
}

QStringList CopyCuesChange::formatsTouched() const
{
    return {m_format};
}

// Every target this copy writes to. The source is only read, so it is
// deliberately absent -- naming it would put an unchanged file in the
// backup record for Undo to restore over.
std::vector<BackupTarget> CopyCuesChange::filesToBackup(SaveContext &ctx) const
{
    std::vector<BackupTarget> targets;
    for (const auto &target : m_op.targets) {
        for (const auto &file :
             filesWrittenFor(WriteScope{}, {m_format.toStdString(), target.sourceId}, m_path, ctx)) {
            targets.push_back({file, "duplicate-cue-consolidation"});
        }
    }
    return targets;
}

ChangeOutcome CopyCuesChange::apply(SaveContext &ctx)
{
    std::string key = "dup:" + m_format.toStdString();
    std::unordered_map<std::string, std::string> oneLibrarySourceIdToPath;
    if (m_format == "onelibrary") {
        oneLibrarySourceIdToPath[m_op.source.sourceId] = m_op.source.filePath;
        for (const auto &target : m_op.targets) {
            oneLibrarySourceIdToPath[target.sourceId] = target.filePath;
        }
        key += ":" + m_groupKey.toStdString();  // the adapter is bound to this group's tracks
    }
    DuplicatesFormatContext &format = ctx.shared<DuplicatesFormatContext>(
        key, [&]() { return makeContext(m_format, m_path, oneLibrarySourceIdToPath); });

    for (const auto &target : m_op.targets) {
        for (const auto &file :
             filesWrittenFor(WriteScope{}, {m_format.toStdString(), target.sourceId}, m_path, ctx)) {
            ctx.backupOnce(file, "duplicate-cue-consolidation");
        }
        format.writer->writeHotCues(target.sourceId, m_op.source.cues);
        ctx.log().record("copied " + std::to_string(m_op.source.cues.size()) + " cue(s) from track id="
                         + m_op.source.sourceId + " to track id=" + target.sourceId);
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
