// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/merge_cues_change.hpp"

#include <filesystem>
#include <memory>
#include <optional>
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

// The writer of one save's merges for one format: the format's cue
// writer plus, for rekordbox, the best-effort OneLibrary mirror. Created
// by the first MergeCuesChange that needs it, shared by the rest
// (SaveContext::shared); the shared database is backed up once here.
// OneLibrary's adapter is bound to the track it writes, so that format
// gets one context per change.
struct LocalCueWriterContext
{
    LocalCueWriterContext(const QString &format, const QString &path, SaveContext &ctx,
                          std::unordered_map<std::string, std::string> oneLibraryPaths)
    {
        std::string root = path.toStdString();
        if (format == "rekordbox") {
            writer = std::make_unique<infrastructure::rekordbox::RekordboxCueWriter>(
                root, sharedAnlzPathIndex(ctx, path));
            // Best-effort secondary write target alongside the primary
            // rekordbox write -- see OneLibraryCueWriter's class comment
            // and docs/onelibrary-format.md.
            if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(root)) {
                ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), "local-restore");
                try {
                    mirror = std::make_unique<infrastructure::onelibrary::OneLibraryCueWriter>(root);
                } catch (const std::exception &e) {
                    ctx.log().record(std::string("local-restore: could not open OneLibrary: ") + e.what());
                }
            }
        } else if (format == "engine") {
            ctx.backupOnce((fs::path(root) / "Database2" / "m.db").string(), "local-restore");
            writer = std::make_unique<infrastructure::engine::LibdjinteropEngineCueWriter>(root);
        } else {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), "local-restore");
            writer = std::make_unique<OneLibraryCueWriterAdapter>(root, std::move(oneLibraryPaths));
        }
    }

    std::unique_ptr<application::CueWriter> writer;
    std::unique_ptr<infrastructure::onelibrary::OneLibraryCueWriter> mirror;
};

}  // namespace

MergeCuesChange::MergeCuesChange(QString format, QString path, domain::RestoreCandidate candidate)
    : m_format(std::move(format)), m_path(std::move(path)), m_candidate(std::move(candidate))
{
}

QString MergeCuesChange::id() const
{
    return "localcue:" + m_format + ":" + QString::fromStdString(m_candidate.stickTrack.sourceId);
}

QString MergeCuesChange::description() const
{
    int added = static_cast<int>(m_candidate.mergedCues.size() - m_candidate.stickTrack.cues.size());
    return QStringLiteral("Merge %1 new cue(s) from the local backup onto \"%2\"")
        .arg(added)
        .arg(QString::fromStdString(m_candidate.stickTrack.title));
}

QString MergeCuesChange::verb() const
{
    return QStringLiteral("merged");
}

QString MergeCuesChange::unit() const
{
    return QStringLiteral("tracks");
}

QStringList MergeCuesChange::formatsTouched() const
{
    return {m_format};
}

// Same shape as RemoveJunkCueChange: the format decides which catalog is
// written, and rekordbox additionally writes this track's own analysis
// file, whose path needs the shared index apply() would use anyway.
std::vector<BackupTarget> MergeCuesChange::filesToBackup(SaveContext &ctx) const
{
    std::vector<BackupTarget> targets;
    const domain::TrackId track{m_format.toStdString(), m_candidate.stickTrack.sourceId};
    for (const auto &file : filesWrittenFor(WriteScope{.catalogRows = false, .oneLibraryMirror = true}, track, m_path, ctx)) {
        targets.push_back({file, "local-restore"});
    }
    return targets;
}

ChangeOutcome MergeCuesChange::apply(SaveContext &ctx)
{
    const domain::Track &track = m_candidate.stickTrack;
    std::string key = "localcue:" + m_format.toStdString();
    std::unordered_map<std::string, std::string> oneLibraryPaths;
    if (m_format == "onelibrary") {
        oneLibraryPaths[track.sourceId] = track.filePath;
        key += ":" + track.sourceId;
    }
    LocalCueWriterContext &writer = ctx.shared<LocalCueWriterContext>(
        key, [&]() { return std::make_unique<LocalCueWriterContext>(m_format, m_path, ctx, oneLibraryPaths); });

    if (m_format == "rekordbox") {
        // rekordbox stores cues per track (ANLZ files): back up this
        // track's own file, same as Sync's rekordbox path.
        std::string root = m_path.toStdString();
        const auto *pathIndex = sharedAnlzPathIndex(ctx, m_path);
        const uint32_t trackId = static_cast<uint32_t>(std::stoul(track.sourceId));
        auto analyzePath = pathIndex ? pathIndex->pathFor(trackId)
                                     : infrastructure::rekordbox::findAnlzPathForTrackId(root, trackId);
        if (analyzePath) {
            ctx.backupOnce(infrastructure::rekordbox::extAnlzPath(root, *analyzePath), "local-restore");
        }
    }

    // mergedCues is the *complete* cue list to end up with -- the
    // stick's own cues, untouched, plus whichever of the backup's cues
    // filled a gap. writeHotCues() replaces the whole set, so passing
    // anything less would silently drop what's already there.
    writer.writer->writeHotCues(track.sourceId, m_candidate.mergedCues);
    int added = static_cast<int>(m_candidate.mergedCues.size() - track.cues.size());
    ctx.log().record("local-restore: merged " + std::to_string(added) + " new cue(s) onto track id=" + track.sourceId
                     + " (\"" + track.title + "\") from local backup");

    if (writer.mirror && !track.filePath.empty()) {
        try {
            writer.mirror->writeCuesForPath(track.filePath, m_candidate.mergedCues);
            ctx.log().record("local-restore: also wrote merged cues into OneLibrary (id=" + track.sourceId + ")");
        } catch (const std::exception &e) {
            ctx.log().record("local-restore: OneLibrary cue write failed for \"" + track.title + "\": " + e.what());
        }
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
