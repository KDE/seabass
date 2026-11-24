// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/sync_plan_change.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <type_traits>
#include <memory>
#include <string>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using domain::SyncPlan;

namespace
{

QString formatLabel(const std::string &format)
{
    if (format == "engine") return "Engine";
    if (format == "onelibrary") return "OneLibrary";
    return "Rekordbox";
}

// The writer for one target catalog in one save, created by the first
// SyncPlanChange that needs it and shared by the rest
// (SaveContext::shared): the catalog's database is backed up once, and a
// big batch against Engine/OneLibrary goes through FormatWriteSession's
// scratch copy. rekordbox cue writes go to small per-track .ANLZ files
// (backed up per item by the change), so its hint is 0: never scratch.
struct SyncFormatWriter
{
    SyncFormatWriter(const std::string &format, const std::string &catalogPath, int itemCountHint, SaveContext &ctx,
                     const infrastructure::rekordbox::AnlzPathIndex *pathIndex)
        : session(sharedFormatWriteSession(ctx, format, catalogPath,
                                            format == "rekordbox" ? 0 : itemCountHint, "sync"))
    {
        if (format == "rekordbox") {
            rekordbox = std::make_unique<infrastructure::rekordbox::RekordboxCueWriter>(session.realRoot(),
                                                                                            pathIndex);
        } else if (format == "engine") {
            engine = std::make_unique<infrastructure::engine::LibdjinteropEngineCueWriter>(session.writeRoot());
            // The scratch copy holds the database, not the audio.
            engine->setAudioRoot(session.realRoot());
            useSaveLeadIns(ctx, *engine, session.realRoot());
        } else {
            // The save's one writer for this database, not a private
            // instance. This branch and the rekordbox branch's mirror
            // (sharedOneLibraryWriter at the write site below) can both
            // run in one save on a stick carrying both catalogs, and two
            // instances against one exportLibrary.db make the second
            // throw as soon as SQLite folds the first's commits into the
            // file -- the very thing sharedOneLibraryWriter exists to
            // stop. realStickRoot is passed explicitly because
            // writeRoot() may be a scratch copy whose parent is a temp
            // directory, and content.path lookups need the real stick.
            oneLibrary = &sharedOneLibraryWriter(ctx, session.writeRoot(),
                                                  pathToUtf8(pathFromUtf8(catalogPath).parent_path()));
        }
    }

    // The save's session for this database, shared with every
    // other change that writes it -- see sharedFormatWriteSession().
    FormatWriteSession &session;
    std::unique_ptr<infrastructure::rekordbox::RekordboxCueWriter> rekordbox;
    std::unique_ptr<infrastructure::engine::LibdjinteropEngineCueWriter> engine;
    // Owned by the save (SaveContext::shared), not by this context.
    infrastructure::onelibrary::OneLibraryCueWriter *oneLibrary = nullptr;
};

// A rekordbox catalog's rows, read once a save, whose cues a check reads
// again from the analysis files when it asks (fillCues), so it sees what
// earlier changes of the same save wrote.
template <class Reader>
struct RowsAsNow
{
    // The live catalog, never a read copy: a save checking what it writes
    // (#110). A rekordbox reader reads export.pdb itself.
    explicit RowsAsNow(const std::string &catalogPath) : reader(makeReader(catalogPath)), rows(reader.readTracks()) {}
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

// A track's cues as its catalog holds them now: Engine's on its own
// timeline, through the save's Engine writer when this save has one (its
// scratch copy included), else read from the library; rekordbox's and
// OneLibrary's from the analysis files. Nothing when the row cannot be
// found or read.
std::optional<std::vector<domain::CuePoint>> cuesNow(SaveContext &ctx, const domain::Track &track,
                                                     const std::string &catalogPath)
{
    try {
        if (track.format == "engine") {
            if (auto *writer = ctx.sharedIfPresent<SyncFormatWriter>("sync:engine"); writer && writer->engine) {
                return writer->engine->storedCues(track.sourceId);
            }
            // Opened for this one read and closed again: a connection held
            // across the save would keep m.db open while a batch written
            // through the scratch copy is renamed over it, which Windows
            // refuses, and the whole save would go back.
            infrastructure::engine::LibdjinteropEngineCueWriter reader(catalogPath);
            return reader.storedCues(track.sourceId);
        }
        const domain::Track &target = track;
        if (target.format == "rekordbox") {
            using Rows = RowsAsNow<infrastructure::rekordbox::KaitaiRekordboxReader>;
            return ctx.shared<Rows>("sync:as-now:rekordbox", [&]() { return std::make_unique<Rows>(catalogPath); })
                .cuesOf(target.sourceId);
        }
        using Rows = RowsAsNow<infrastructure::onelibrary::OneLibraryReader>;
        return ctx.shared<Rows>("sync:as-now:onelibrary", [&]() { return std::make_unique<Rows>(catalogPath); })
            .cuesOf(target.sourceId);
    } catch (const std::exception &) {
        return std::nullopt;  // unreadable is not "as read": nothing is written
    }
}

constexpr double AsReadWithinMs = 1.0;

// `cues` of `track` on the timeline its catalog stores them on: Engine's
// were read onto rekordbox's, by the shift the reader added.
std::vector<domain::CuePoint> asStored(const domain::Track &track, std::vector<domain::CuePoint> cues)
{
    if (track.format == "engine") {
        for (domain::CuePoint &cue : cues) {
            cue.positionMs -= track.cueTimelineShiftMs;
            if (cue.isLoop) {
                cue.loopEndMs -= track.cueTimelineShiftMs;
            }
        }
    }
    return cues;
}

// Whether `now` is still what was read for `track`.
bool sameCuesAsReadFor(const domain::Track &track, const std::vector<domain::CuePoint> &now)
{
    return domain::sameCuesAsRead(asStored(track, track.cues), now, AsReadWithinMs, !track.cueRateUnverified);
}

// Whether every one of `cues` (of `track`, as read) is still in `now`.
bool stillHolds(const domain::Track &track, const std::vector<domain::CuePoint> &cues,
                const std::vector<domain::CuePoint> &now)
{
    for (const domain::CuePoint &cue : asStored(track, cues)) {
        const bool held = std::any_of(now.begin(), now.end(), [&](const domain::CuePoint &n) {
            return n.kind == cue.kind && n.hotCueNumber == cue.hotCueNumber && n.isLoop == cue.isLoop
                && (track.cueRateUnverified
                    || (std::abs(n.positionMs - cue.positionMs) <= AsReadWithinMs
                        && (!cue.isLoop || std::abs(n.loopEndMs - cue.loopEndMs) <= AsReadWithinMs)));
        });
        if (!held) {
            return false;
        }
    }
    return true;
}

}  // namespace

SyncPlanChange::SyncPlanChange(QString rekordboxPath, QString enginePath, SyncPlan plan, int itemCountHint)
    : m_rekordboxPath(std::move(rekordboxPath)),
      m_enginePath(std::move(enginePath)),
      m_plan(std::move(plan)),
      m_itemCountHint(itemCountHint)
{
}

SyncPlanChange::~SyncPlanChange() = default;

void SyncPlanChange::setAnalysisRider(std::unique_ptr<PendingChange> rider)
{
    m_rider = std::move(rider);
}

const domain::Track &SyncPlanChange::target() const
{
    return m_plan.direction == SyncPlan::Direction::ToB ? m_plan.match.trackB : m_plan.match.trackA;
}

const domain::Track &SyncPlanChange::source() const
{
    return m_plan.direction == SyncPlan::Direction::ToB ? m_plan.match.trackA : m_plan.match.trackB;
}

QString SyncPlanChange::id() const
{
    return "sync:" + QString::fromStdString(target().format) + ":" + QString::fromStdString(target().sourceId);
}

QString SyncPlanChange::description() const
{
    return QStringLiteral("Copy %1 from %2 \"%3\" to %4")
        .arg(describeCues(m_plan.cuesToApply), formatLabel(source().format), QString::fromStdString(source().title),
             formatLabel(target().format));
}

// The track, as the page lists it: the progress dialog names what is
// being written, one line that does not change the dialog's size.
QString SyncPlanChange::subject() const
{
    const domain::Track &track = source();
    const QString title = QString::fromStdString(track.title.empty() ? track.filename : track.title);
    return track.artist.empty() ? title : title + QStringLiteral(", ") + QString::fromStdString(track.artist);
}

QString SyncPlanChange::verb() const
{
    return QStringLiteral("synchronised");
}

QString SyncPlanChange::unit() const
{
    return QStringLiteral("tracks");
}

QStringList SyncPlanChange::formatsTouched() const
{
    return {QString::fromStdString(target().format)};
}

// Only the target is written; the source is read. A rekordbox target also
// writes the OneLibrary copy of that same library -- see apply().
std::vector<BackupTarget> SyncPlanChange::filesToBackup(SaveContext &ctx) const
{
    const domain::Track &tgt = target();
    const QString path = tgt.format == "engine" ? m_enginePath : m_rekordboxPath;
    if (path.isEmpty()) {
        return {};  // apply() reports the missing catalog path
    }
    std::vector<BackupTarget> targets;
    const WriteScope scope{.cueData = true, .catalogRows = false, .oneLibraryMirror = true};
    for (const auto &file : filesWrittenFor(scope, {tgt.format, tgt.sourceId}, path, ctx)) {
        targets.push_back({file, "sync"});
    }
    // The rider's (its kept analysis and the ledger), each file once: the
    // database is this change's already.
    if (m_rider) {
        for (auto &target : m_rider->filesToBackup(ctx)) {
            const bool declared = std::any_of(targets.begin(), targets.end(),
                                              [&](const BackupTarget &t) { return t.file == target.file; });
            if (!declared) {
                targets.push_back(std::move(target));
            }
        }
    }
    return targets;
}

void SyncPlanChange::beforeSave(SaveContext &ctx)
{
    if (m_rider) {
        m_rider->beforeSave(ctx);
    }
}

// The rekordbox target's whole set is the plan's cues, which apply()
// hands its writer (and the OneLibrary mirror) as they are. A track with
// no file to name has nothing the baseline could key it on.
std::vector<RekordboxWrite> SyncPlanChange::rekordboxWrites() const
{
    const domain::Track &tgt = target();
    if ((tgt.format != "rekordbox" && tgt.format != "onelibrary") || tgt.filePath.empty()) {
        return {};
    }
    return {RekordboxWrite{tgt.filePath, m_plan.cuesToApply, std::nullopt}};
}

std::string SyncPlanChange::logLine() const
{
    const domain::Track &tgt = target();
    const domain::Track &src = source();
    return "sync: copied cues (" + describeCues(m_plan.cuesToApply).toStdString() + ") from " + src.format
        + " track \"" + src.title + "\" (id=" + src.sourceId + ") to " + tgt.format + " track id=" + tgt.sourceId;
}

ChangeOutcome SyncPlanChange::apply(SaveContext &ctx)
{
    const domain::Track &tgt = target();
    const std::string &targetFormat = tgt.format;
    // rekordbox and OneLibrary share the same PIONEER root/path.
    QString path = targetFormat == "engine" ? m_enginePath : m_rekordboxPath;
    if (path.isEmpty()) {
        return ChangeOutcome::failure("No " + formatLabel(targetFormat) + " catalog path is known for this stick.");
    }
    std::string catalogPath = path.toStdString();
    // The analysis-path index is a rekordbox thing: building it for an
    // Engine target looked for export.pdb under the Engine Library and
    // logged a failure on every sync onto Engine.
    SyncFormatWriter &writer = ctx.shared<SyncFormatWriter>("sync:" + targetFormat, [&]() {
        return std::make_unique<SyncFormatWriter>(targetFormat, catalogPath, m_itemCountHint, ctx,
                                                 targetFormat == "rekordbox" ? sharedAnlzPathIndex(ctx, path)
                                                                             : nullptr);
    });

    if (m_checkTargetAsRead) {
        // Both sides: the target is written over, and what goes onto it is
        // the source's cues as they were read.
        const domain::Track &src = source();
        const std::string sourcePath = (src.format == "engine" ? m_enginePath : m_rekordboxPath).toStdString();
        const auto targetNow = cuesNow(ctx, tgt, catalogPath);
        const auto sourceNow = cuesNow(ctx, src, sourcePath);
        const bool targetAsRead = targetNow && sameCuesAsReadFor(tgt, *targetNow);
        const std::optional<std::vector<domain::CuePoint>> copied = sourceCuesCopied();
        const bool sourceAsRead =
            sourceNow && (copied ? stillHolds(src, *copied, *sourceNow) : sameCuesAsReadFor(src, *sourceNow));
        if (!targetAsRead || !sourceAsRead) {
            const QString title = QString::fromStdString(tgt.title.empty() ? tgt.filename : tgt.title);
            // Which side, and whether it could not be read at all (a read
            // error, or the row gone) or holds other cues than were read.
            const bool unreadable = !targetNow || !sourceNow;
            const std::string &side = (!targetNow || (targetNow && !targetAsRead)) ? targetFormat : src.format;
            const QString why = unreadable
                ? QStringLiteral("its cues on %1 could not be read, or the track is no longer there")
                : QStringLiteral("its cues on %1 changed after they were read");
            ctx.addSummaryNote(QStringLiteral("\"%1\" was left alone: %2.").arg(title, why.arg(formatLabel(side))));
            ctx.log().record("sync: left track id=" + tgt.sourceId + " on " + targetFormat + " alone: "
                             + (unreadable ? "the cues on " + side + " could not be read"
                                           : "the cues on " + side + " changed after they were read"));
            return ChangeOutcome::skip();
        }
    }

    if (targetFormat == "rekordbox") {
        const auto *pathIndex = sharedAnlzPathIndex(ctx, path);
        const uint32_t trackId = static_cast<uint32_t>(std::stoul(tgt.sourceId));
        auto analyzePath = pathIndex ? pathIndex->pathFor(trackId)
                                     : infrastructure::rekordbox::findAnlzPathForTrackId(catalogPath, trackId);
        if (analyzePath) {
            for (const auto &file : infrastructure::rekordbox::rekordboxCueFilesFor(catalogPath, *analyzePath)) {
                ctx.backupOnce(file, "sync");
            }
        }
        writer.rekordbox->writeHotCues(tgt.sourceId, m_plan.cuesToApply);
        // export.pdb and exportLibrary.db are the SAME library in two
        // formats, not two catalogs, so a cue written to one and not the
        // other leaves rekordbox 7 showing a track the older export says
        // has cues and this one says does not. Every other cue-writing
        // workflow mirrors here; Sync did not, which is what made a
        // rekordbox sync a two-way sync on a three-format stick.
        //
        // A mirror failure fails the change. It used to be logged while
        // the save reported success -- the argument being that a stale
        // copy you can re-sync beats a failed save -- but nobody re-syncs
        // a library they were told had synced. Failing the change also
        // takes the DeviceLibrary half back out -- the save loop restores
        // every file the change declared -- so the stick is left as it was
        // before this track, not half synced.
        //
        // Through mirrorCuesOrExplain() rather than a writeCuesForPath()
        // of its own, because writing blind here was refusing syncs that
        // are perfectly fine (#14). A track Device Library Plus does not
        // list has no copy to keep in step, so there is nothing to
        // disagree -- but contentIdsAt() throws OneLibraryRowMissing for
        // it, the catch below took that for a mirror failure, and the
        // save loop then rolled back the rekordbox cue write that had
        // already succeeded. On the hardware capture in #14, 635 of 1118
        // tracks have no OneLibrary row, so this was not an edge case:
        // syncing any of them failed the whole save and undid itself.
        //
        // mirrorCuesOrExplain() asks hasTrackAtPath() first and treats
        // "no row" as nothing to do, which is what it is. A write that
        // genuinely fails still fails the change, and the log now
        // separates the two -- "also wrote into OneLibrary" against
        // "does not list this file; nothing to mirror" -- so the line
        // #14 wanted to assert on can actually carry that weight.
        if (!tgt.filePath.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(catalogPath)) {
            const QString failed =
                mirrorCuesOrExplain(sharedOneLibraryWriter(ctx, catalogPath), tgt.filePath, m_plan.cuesToApply, ctx,
                                    "sync", QStringLiteral("write the cues for \"%1\"")
                                                .arg(QString::fromStdString(tgt.title)));
            if (!failed.isEmpty()) {
                return ChangeOutcome::failure(failed);
            }
        }
    } else if (targetFormat == "engine") {
        writer.engine->writeHotCues(tgt.sourceId, m_plan.cuesToApply);
    } else {
        writer.oneLibrary->writeCuesForPath(tgt.filePath, m_plan.cuesToApply);
    }
    writer.session.noteItemApplied();
    ctx.log().record(logLine());
    // rekordbox's analysis onto the row whose cues were just written. What
    // it left alone is a skip of its own, said in the summary note; the
    // cues stand either way. A failure fails this change, cues included.
    if (m_rider) {
        const ChangeOutcome analysis = m_rider->apply(ctx);
        if (!analysis.ok) {
            return analysis;
        }
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
