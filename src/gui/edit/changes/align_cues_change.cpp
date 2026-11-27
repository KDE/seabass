// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/align_cues_change.hpp"

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "domain/sync_planning.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
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

// A rekordbox catalog's rows, read once a save, whose cues are read again
// from the analysis files when asked, so a check sees what the stick
// holds now (as SyncPlanChange's check of a target reads them).
template <class Reader>
struct RowsNow
{
    explicit RowsNow(const std::string &catalogPath) : reader(makeReader(catalogPath)), rows(reader.readTracks()) {}
    static Reader makeReader(const std::string &catalogPath)
    {
        if constexpr (std::is_same_v<Reader, infrastructure::onelibrary::OneLibraryReader>) {
            return Reader(catalogPath, infrastructure::ReadFor::Writing);
        } else {
            return Reader(catalogPath);
        }
    }
    std::optional<std::vector<domain::CuePoint>> cuesOf(const std::string &sourceId)
    {
        for (const domain::Track &row : rows) {
            if (row.sourceId == sourceId) {
                std::vector<domain::Track> one{row};
                reader.fillCues(one);
                return one.front().cues;
            }
        }
        return std::nullopt;
    }
    Reader reader;
    std::vector<domain::Track> rows;
};

// A stored position and the one read may differ by how it is kept (whole
// milliseconds, samples).
constexpr double AsReadWithinMs = 1.0;

}  // namespace

AlignCuesChange::AlignCuesChange(QString path, domain::Track track, std::vector<domain::CuePoint> aligned,
                                 double gridShiftMs, int moved)
    : m_path(std::move(path)), m_track(std::move(track)), m_aligned(std::move(aligned)), m_gridShiftMs(gridShiftMs),
      m_moved(moved), m_written(moved)
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
    return m_written;
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
    m_written = m_moved;

    // The cues the stick holds now, on the timeline `m_aligned` is on, and
    // the ones the page read, on the same: unless they are the same cues,
    // nothing is written. What this change holds is the list as it was
    // read, and it would go over whatever was added or moved since.
    std::optional<std::vector<domain::CuePoint>> now;
    std::vector<domain::CuePoint> asRead = m_track.cues;
    try {
        if (format == "engine") {
            now = w.engine->storedCues(m_track.sourceId);
            for (domain::CuePoint &cue : asRead) {
                cue.positionMs -= m_gridShiftMs;
                if (cue.isLoop) {
                    cue.loopEndMs -= m_gridShiftMs;
                }
            }
        } else if (format == "rekordbox") {
            using Rows = RowsNow<infrastructure::rekordbox::KaitaiRekordboxReader>;
            now = ctx.shared<Rows>(std::string(Label) + ":now:rekordbox", [&]() { return std::make_unique<Rows>(root); })
                      .cuesOf(m_track.sourceId);
        } else if (format == "onelibrary") {
            using Rows = RowsNow<infrastructure::onelibrary::OneLibraryReader>;
            now = ctx.shared<Rows>(std::string(Label) + ":now:onelibrary", [&]() { return std::make_unique<Rows>(root); })
                      .cuesOf(m_track.sourceId);
        } else {
            return ChangeOutcome::failure("Unknown library format: " + format);
        }
    } catch (const std::exception &) {
        now.reset();  // unreadable is not "as read": nothing is written
    }
    if (!now || !domain::sameCuesAsRead(asRead, *now, AsReadWithinMs, true)) {
        m_written = 0;
        const QString title = QString::fromStdString(m_track.title.empty() ? m_track.filename : m_track.title);
        ctx.addSummaryNote(now ? QStringLiteral("\"%1\" was left alone: its cue points changed after they were read.").arg(title)
                               : QStringLiteral("\"%1\" was left alone: its cue points could not be read again.").arg(title));
        ctx.log().record(std::string(Label) + ": left \"" + m_track.title + "\" (" + m_track.format + ", id=" + m_track.sourceId
                         + ") alone: " + (now ? "its cues changed after they were read" : "its cues could not be read again"));
        return ChangeOutcome::skip();
    }

    if (format == "rekordbox") {
        const std::vector<infrastructure::rekordbox::MovedCue> moved = infrastructure::rekordbox::movedCues(m_track.cues, m_aligned);
        const auto *pathIndex = sharedAnlzPathIndex(ctx, m_path);
        const uint32_t trackId = static_cast<uint32_t>(std::stoul(m_track.sourceId));
        const auto analyzePath = pathIndex ? pathIndex->pathFor(trackId)
                                           : infrastructure::rekordbox::findAnlzPathForTrackId(root, trackId);
        if (analyzePath) {
            for (const auto &file : infrastructure::rekordbox::rekordboxCueFilesFor(root, *analyzePath)) {
                ctx.backupOnce(file, Label);
            }
        }
        w.rekordbox->writeHotCuesMoved(m_track.sourceId, m_aligned, moved);
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
                    oneLibrary.writeCuesForPath(m_track.filePath, m_aligned, moved);
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
        // As Engine stores them: no offset taken off, the row's mark left
        // as it is (a marked row is never listed).
        w.engine->writeEngineStoredCues(m_track.sourceId, m_aligned);
        w.engineSession->noteItemApplied();
    } else {
        w.oneLibrary->writeCuesForPath(m_track.filePath, m_aligned, infrastructure::rekordbox::movedCues(m_track.cues, m_aligned));
    }
    ctx.log().record(std::string(Label) + ": moved " + std::to_string(m_moved) + " cue point(s) of \"" + m_track.title
                     + "\" (" + m_track.format + ", id=" + m_track.sourceId + ") onto the beat grid");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
