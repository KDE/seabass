// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/remove_junk_cue_change.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/junk_cue.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

std::vector<domain::CuePoint> cuesWithoutJunk(const domain::Track &track)
{
    std::vector<domain::CuePoint> remainingCues;
    for (const auto &c : track.cues) {
        // The same rule the finder uses (domain::JunkCueFinder): a memory
        // cue inside the first second. The remover used to test == 0.0,
        // so a cue at 12 ms was listed, "removed", and survived the
        // rewrite.
        if (!domain::isJunkCue(c)) {
            remainingCues.push_back(c);
        }
    }
    return remainingCues;
}

// The writer of one save's stray-cue removals for one format.
struct JunkCueWriterContext
{
    JunkCueWriterContext(const QString &format, const QString &path, SaveContext &ctx)
    {
        std::string root = path.toStdString();
        if (format == "rekordbox") {
            // export.pdb is deliberately NOT backed up here: cue data lives
            // entirely in the per-track ANLZ files and this path never
            // writes export.pdb (see RekordboxCueWriter's own header). The
            // OneLibrary mirror below IS written, so it is what needs the
            // backup -- without it Undo restored the analysis file and left
            // the mirror holding the removed cue.
            rekordbox = std::make_unique<infrastructure::rekordbox::RekordboxCueWriter>(
                root, sharedAnlzPathIndex(ctx, path));
            hasOneLibrary = infrastructure::onelibrary::OneLibraryCueWriter::existsFor(root);
            if (hasOneLibrary) {
                ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), "junk-cue-cleanup");
            }
        } else if (format == "engine") {
            // Through the save's shared FormatWriteSession for this
            // database, like RepairIssueChange: Library Health stages
            // both kinds of change into one save, and when the repairs
            // moved m.db to a scratch copy, junk-cue writes made straight
            // to the stick were overwritten by the scratch's commit.
            engineSession = &sharedFormatWriteSession(ctx, "engine", root, 0, "junk-cue-cleanup");
            engine = std::make_unique<infrastructure::engine::LibdjinteropEngineCueWriter>(engineSession->writeRoot());
        } else {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), "junk-cue-cleanup");
            // The save's shared writer, not one of this change's own: a
            // second instance against the same database pays another key
            // derivation, trips its own staleness guard, and is never
            // checkpointed -- its rows reach the file only when SQLite
            // folds the log at close.
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

RemoveJunkCueChange::RemoveJunkCueChange(QString path, domain::Track track)
    : m_path(std::move(path)), m_track(std::move(track))
{
}

QString RemoveJunkCueChange::id() const
{
    return "junk:" + junkKeyFor(m_track);
}

QString RemoveJunkCueChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RemoveJunkCueChange::description() const
{
    return QStringLiteral("Remove the cue at 0:00 from \"%1\"").arg(QString::fromStdString(m_track.title));
}

QString RemoveJunkCueChange::verb() const
{
    return QStringLiteral("removed");
}

QString RemoveJunkCueChange::unit() const
{
    return QStringLiteral("cues");
}

int RemoveJunkCueChange::unitsWritten() const
{
    return static_cast<int>(m_track.cues.size() - cuesWithoutJunk(m_track).size());
}

QStringList RemoveJunkCueChange::formatsTouched() const
{
    return {QString::fromStdString(m_track.format)};
}

// Known before the save runs: the analysis file for this track, plus the
// catalog its format writes. The same paths apply() would have backed up
// one at a time, so this only moves when it happens -- everything before
// the first overwrite instead of interleaved with it.
//
// export.pdb is deliberately absent, as in apply(): cue data lives in the
// per-track ANLZ files and this path never writes it.
std::vector<BackupTarget> RemoveJunkCueChange::filesToBackup(SaveContext &ctx) const
{
    // Cues only: this path never writes export.pdb (see
    // RekordboxCueWriter's own header), so the catalog must not be named.
    std::vector<BackupTarget> targets;
    for (const auto &file : filesWrittenFor(WriteScope{.catalogRows = false, .oneLibraryMirror = true}, {m_track.format, m_track.sourceId}, m_path, ctx)) {
        targets.push_back({file, "junk-cue-cleanup"});
    }
    return targets;
}

ChangeOutcome RemoveJunkCueChange::apply(SaveContext &ctx)
{
    const QString format = QString::fromStdString(m_track.format);
    std::string root = m_path.toStdString();
    JunkCueWriterContext &w = ctx.shared<JunkCueWriterContext>(
        "junk:" + m_track.format, [&]() { return std::make_unique<JunkCueWriterContext>(format, m_path, ctx); });
    auto remainingCues = cuesWithoutJunk(m_track);

    if (format == "rekordbox") {
        const auto *pathIndex = sharedAnlzPathIndex(ctx, m_path);
        const uint32_t trackId = static_cast<uint32_t>(std::stoul(m_track.sourceId));
        auto analyzePath = pathIndex ? pathIndex->pathFor(trackId)
                                     : infrastructure::rekordbox::findAnlzPathForTrackId(root, trackId);
        if (analyzePath) {
            for (const auto &file : infrastructure::rekordbox::rekordboxCueFilesFor(root, *analyzePath)) {
                ctx.backupOnce(file, "junk-cue-cleanup");
            }
        }
        w.rekordbox->writeHotCues(m_track.sourceId, remainingCues);
        // export.pdb and exportLibrary.db are one library in two formats,
        // so a cue removed from one and not the other is a library that
        // disagrees with itself. This used to be logged and the change
        // reported success: the page said the junk cue was gone while a
        // player reading Device Library Plus still showed it, and Undo
        // offered nothing, because nothing had failed. AddCueChange was
        // fixed for exactly this (see its own comment) and this is the
        // same write; failing here also takes the DeviceLibrary half back
        // out, because the save loop restores every file this change
        // declared.
        //
        // A file OneLibrary does not list is not a disagreement: there is
        // no Device Library Plus copy to keep in step. Asked before the
        // write, as AddCueChange asks it, rather than inferred from the
        // exception -- 635 of 1118 tracks on a real stick are in that
        // position and none of them is a failure.
        if (w.hasOneLibrary && !m_track.filePath.empty()) {
            try {
                auto &oneLibrary = sharedOneLibraryWriter(ctx, root);
                if (!oneLibrary.hasTrackAtPath(m_track.filePath)) {
                    ctx.log().record("junk-cue: OneLibrary does not list this file; nothing to mirror");
                } else {
                    ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(root), "junk-cue-cleanup");
                    oneLibrary.writeCuesForPath(m_track.filePath, remainingCues);
                }
            } catch (const std::exception &e) {
                ctx.log().record(std::string("junk-cue: OneLibrary cue mirror failed: ") + e.what());
                return ChangeOutcome::failure(
                    QStringLiteral("Could not remove the cue from Device Library Plus: %1. The save stops here "
                                   "and puts back what this change wrote, so DeviceLibrary and Device Library "
                                   "Plus stay in agreement.")
                        .arg(QString::fromUtf8(e.what())));
            }
        }
    } else if (format == "engine") {
        w.engine->writeHotCues(m_track.sourceId, remainingCues);
        w.engineSession->noteItemApplied();
    } else if (format == "onelibrary") {
        w.oneLibrary->writeCuesForPath(m_track.filePath, remainingCues);
    } else {
        return ChangeOutcome::failure("Unknown library format: " + format);
    }
    ctx.log().record("junk-cue: removed 0:00 memory cue from \"" + m_track.title + "\" (id=" + m_track.sourceId + ")");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
