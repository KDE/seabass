// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/align_cues_change.hpp"

#include <memory>
#include <string>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::gui
{

namespace
{

constexpr const char *Label = "align-cues";

// The writers of one save's aligned cues for one format, as
// RemoveJunkCueChange has them for its removals (see its comments for why
// each is opened the way it is: the cue data is in the analysis files and
// export.pdb is never written, Engine goes through the save's shared
// write session, OneLibrary through the save's shared writer).
struct AlignCuesWriterContext
{
    AlignCuesWriterContext(const QString &format, const QString &path, SaveContext &ctx)
    {
        const std::string root = path.toStdString();
        if (format == "rekordbox") {
            rekordbox = std::make_unique<infrastructure::rekordbox::RekordboxCueWriter>(root, sharedAnlzPathIndex(ctx, path));
            hasOneLibrary = infrastructure::onelibrary::OneLibraryCueWriter::existsFor(root);
            if (hasOneLibrary) {
                ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), Label);
            }
        } else if (format == "engine") {
            engineSession = &sharedFormatWriteSession(ctx, "engine", root, 0, Label);
            engine = std::make_unique<infrastructure::engine::LibdjinteropEngineCueWriter>(engineSession->writeRoot());
            // The scratch copy holds the database, not the audio.
            engine->setAudioRoot(engineSession->realRoot());
            useSaveLeadIns(ctx, *engine, engineSession->realRoot());
        } else {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), Label);
            oneLibrary = &sharedOneLibraryWriter(ctx, root);
        }
    }

    std::unique_ptr<infrastructure::rekordbox::RekordboxCueWriter> rekordbox;
    std::unique_ptr<infrastructure::engine::LibdjinteropEngineCueWriter> engine;
    FormatWriteSession *engineSession = nullptr;
    // Owned by the save (SaveContext::shared), not by this context.
    infrastructure::onelibrary::OneLibraryCueWriter *oneLibrary = nullptr;
    bool hasOneLibrary = false;
};

}  // namespace

AlignCuesChange::AlignCuesChange(QString path, domain::Track track, std::vector<domain::CuePoint> aligned, int moved)
    : m_path(std::move(path)), m_track(std::move(track)), m_aligned(std::move(aligned)), m_moved(moved)
{
}

QString AlignCuesChange::idFor(const domain::Track &track)
{
    return "align-cues:" + junkKeyFor(track);
}

QString AlignCuesChange::id() const
{
    return idFor(m_track);
}

QString AlignCuesChange::owner() const
{
    return QStringLiteral("align-cues");
}

QString AlignCuesChange::description() const
{
    return m_moved == 1 ? QStringLiteral("Move 1 cue point of \"%1\" onto its beat").arg(QString::fromStdString(m_track.title))
                        : QStringLiteral("Move %1 cue points of \"%2\" onto their beats")
                              .arg(m_moved).arg(QString::fromStdString(m_track.title));
}

QString AlignCuesChange::subject() const
{
    return QString::fromStdString(m_track.title);
}

QString AlignCuesChange::unit() const
{
    return QStringLiteral("cue points");
}

int AlignCuesChange::unitsWritten() const
{
    return m_moved;
}

QString AlignCuesChange::verb() const
{
    return QStringLiteral("aligned");
}

QStringList AlignCuesChange::formatsTouched() const
{
    return {QString::fromStdString(m_track.format)};
}

std::vector<BackupTarget> AlignCuesChange::filesToBackup(SaveContext &ctx) const
{
    // Cues only: export.pdb is never written (see RekordboxCueWriter).
    std::vector<BackupTarget> targets;
    for (const auto &file : filesWrittenFor(WriteScope{.catalogRows = false, .oneLibraryMirror = true},
                                            {m_track.format, m_track.sourceId}, m_path, ctx)) {
        targets.push_back({file, Label});
    }
    return targets;
}

ChangeOutcome AlignCuesChange::apply(SaveContext &ctx)
{
    const QString format = QString::fromStdString(m_track.format);
    const std::string root = m_path.toStdString();
    AlignCuesWriterContext &w = ctx.shared<AlignCuesWriterContext>(
        std::string(Label) + ":" + m_track.format, [&]() { return std::make_unique<AlignCuesWriterContext>(format, m_path, ctx); });

    if (format == "rekordbox") {
        const auto *pathIndex = sharedAnlzPathIndex(ctx, m_path);
        const uint32_t trackId = static_cast<uint32_t>(std::stoul(m_track.sourceId));
        const auto analyzePath = pathIndex ? pathIndex->pathFor(trackId)
                                           : infrastructure::rekordbox::findAnlzPathForTrackId(root, trackId);
        if (analyzePath) {
            for (const auto &file : infrastructure::rekordbox::rekordboxCueFilesFor(root, *analyzePath)) {
                ctx.backupOnce(file, Label);
            }
        }
        w.rekordbox->writeHotCues(m_track.sourceId, m_aligned);
        // export.pdb and exportLibrary.db are one library in two formats:
        // a cue moved in one and not the other is a library that disagrees
        // with itself, so a mirror that fails fails the change, and the
        // save puts back what it wrote. A file OneLibrary does not list
        // has no copy to keep in step (as RemoveJunkCueChange).
        if (w.hasOneLibrary && !m_track.filePath.empty()) {
            try {
                auto &oneLibrary = sharedOneLibraryWriter(ctx, root);
                if (!oneLibrary.hasTrackAtPath(m_track.filePath)) {
                    ctx.log().record(std::string(Label) + ": OneLibrary does not list this file; nothing to mirror");
                } else {
                    ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), Label);
                    oneLibrary.writeCuesForPath(m_track.filePath, m_aligned);
                }
            } catch (const std::exception &e) {
                ctx.log().record(std::string(Label) + ": OneLibrary cue mirror failed: " + e.what());
                return ChangeOutcome::failure(
                    QStringLiteral("Could not move the cue points in Device Library Plus: %1. The save stops here "
                                   "and puts back what this change wrote, so DeviceLibrary and Device Library "
                                   "Plus stay in agreement.")
                        .arg(QString::fromUtf8(e.what())));
            }
        }
    } else if (format == "engine") {
        w.engine->writeHotCues(m_track.sourceId, m_aligned);
        w.engineSession->noteItemApplied();
    } else if (format == "onelibrary") {
        w.oneLibrary->writeCuesForPath(m_track.filePath, m_aligned);
    } else {
        return ChangeOutcome::failure("Unknown library format: " + format);
    }
    ctx.log().record(std::string(Label) + ": moved " + std::to_string(m_moved) + " cue point(s) of \"" + m_track.title
                     + "\" (" + m_track.format + ", id=" + m_track.sourceId + ") onto the beat grid");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
