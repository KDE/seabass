// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/restore_metadata_change.hpp"

#include <filesystem>
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
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

constexpr const char *LogTag = "metadata-restore";

// One format's writer for a whole save: the format's cue writer plus,
// for rekordbox, the best-effort OneLibrary mirror. Created by the first
// change that needs it and shared by the rest (SaveContext::shared), so
// a restore of four hundred tracks opens each database once rather than
// four hundred times.
struct RestoreWriterContext
{
    // The hint comes from the controller, which knows how many tracks
    // it is staging. It matters: at a hint of 1 the session never earns
    // a scratch copy, and every rated track then pays a whole-file
    // durable rewrite of export.pdb onto the stick -- 400 of them on a
    // full restore, against one at the end with a copy.
    //
    // The session matters even when it decides against a scratch copy,
    // because it is shared: it is how this change and every other change
    // writing the same database in the same save agree on ONE file to
    // write and one commit. Writing the real database directly while
    // another change has it redirected to a scratch copy loses whatever
    // this wrote, the moment that copy is committed back -- and the page
    // reports the restore as applied.
    RestoreWriterContext(const QString &format, const QString &path, SaveContext &ctx, int itemCountHint)
        // Hint 0 for OneLibrary: this feature writes that format's
        // database directly, so it must never be the change that talks
        // the session into a scratch copy nobody's writes would reach.
        : session(sharedFormatWriteSession(ctx, format.toStdString(), path.toStdString(),
                                            format == "onelibrary" ? 0 : itemCountHint, LogTag))
    {
        // The real catalog folder. rekordbox needs it even when the
        // session redirects: cues live in per-track ANLZ files next to
        // the audio, not in export.pdb, so the scratch copy -- which
        // holds the database and nothing else -- is not where they go.
        // Same split makeContext() draws in Clean Up.
        const std::string realRoot = path.toStdString();
        // The database, wherever the session decided it should be
        // written. Nothing here backs it up: the session did that before
        // handing the root out, and it is the thing that commits it.
        const std::string dbRoot = session.writeRoot();

        if (format == "rekordbox") {
            writer = std::make_unique<infrastructure::rekordbox::RekordboxCueWriter>(
                realRoot, sharedAnlzPathIndex(ctx, path));
            // The two rekordbox formats are one library, so a cue
            // written to one and not the other leaves them disagreeing.
            // Best-effort: see OneLibraryCueWriter's class comment.
            if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(realRoot)) {
                try {
                    // The real PIONEER root, like every other mirror
                    // call site, and NOT this database's write session --
                    // which was tried, and loses the write.
                    //
                    // exportLibrary.db runs in WAL mode. While a writer
                    // connection is open, its committed rows live in
                    // exportLibrary.db-wal, and FormatWriteSession
                    // commits by copying the single .db file back. The
                    // save holds its writers open across that copy, so
                    // anything written through a scratch copy is left
                    // behind in a -wal file nobody copies. Measured, not
                    // reasoned: PRAGMA journal_mode returns "wal" on the
                    // committed fixture, and a write through a scratching
                    // session reads back empty afterwards.
                    //
                    // So the scratch path is the unsafe one for this
                    // format, whichever way round the sharing argument
                    // goes. See docs/metadata-backup-plan.md.
                    mirror = &sharedOneLibraryWriter(ctx, realRoot,
                                                      fs::path(realRoot).parent_path().string());
                } catch (const std::exception &e) {
                    ctx.log().record(std::string(LogTag) + ": could not open OneLibrary: " + e.what());
                }
            }
        } else if (format == "engine") {
            writer = std::make_unique<infrastructure::engine::LibdjinteropEngineCueWriter>(dbRoot);
        } else {
            // OneLibrary writes go to the REAL root, never to a scratch
            // copy, for the WAL reason spelled out on the mirror above:
            // a scratch copy commits the .db file alone and leaves the
            // rows behind in the -wal file. Engine's m.db is a
            // rollback-journal database and has no such problem, which
            // is why only this branch differs.
            //
            // Empty map: each change registers its own track through
            // notePath() as it applies, so one adapter serves the save,
            // and it writes through the save's shared writer so two
            // features cannot open two instances against one file.
            auto adapter = std::make_unique<OneLibraryCueWriterAdapter>(
                realRoot, std::unordered_map<std::string, std::string>{},
                fs::path(realRoot).parent_path().string());
            adapter->useSharedWriter(
                sharedOneLibraryWriter(ctx, realRoot, fs::path(realRoot).parent_path().string()));
            writer = std::move(adapter);
        }
    }

    FormatWriteSession &session;
    std::unique_ptr<application::CueWriter> writer;
    // Owned by the save (SaveContext::shared), not by this context.
    infrastructure::onelibrary::OneLibraryCueWriter *mirror = nullptr;
};

// Writes the rating and the comment, per format, and says in the log
// what each format could not take rather than leaving a gap.
//
// The per-format table is measured, not assumed -- see
// docs/metadata-backup-plan.md and tests/pdb_rating_write_test.cpp:
// Engine and OneLibrary take both fields; export.pdb takes the rating
// (one byte, already there) and cannot take a comment it does not
// already have room for.
// What one track's annotation write did. `ok` false means a write was
// attempted and is known NOT to have reached the catalog, so the change
// must report a failure rather than let the page take the track off its
// list. `wroteToWriteRoot` says whether anything actually went into the
// SESSION's write root -- which is what decides whether a cancelled save
// may throw its scratch copy away. A write to some other database (the
// OneLibrary mirror, which never goes through this format's session)
// does not count towards it.
struct AnnotationOutcome
{
    bool ok = true;
    bool wroteToWriteRoot = false;
};

AnnotationOutcome applyAnnotation(SaveContext &ctx, RestoreWriterContext &writer, const QString &format, const QString &path,
                      const std::string &sourceId, const domain::MetadataRestoreProposal &proposal)
{
    const std::optional<int> stars = proposal.ratingOffered ? proposal.rating : std::nullopt;
    const std::optional<std::string> comment =
        proposal.commentOffered ? std::optional<std::string>(proposal.comment) : std::nullopt;
    if (!stars && !comment) {
        return {};
    }

    if (format == "engine") {
        auto *engine = dynamic_cast<infrastructure::engine::LibdjinteropEngineCueWriter *>(writer.writer.get());
        if (!engine) {
            ctx.log().record(std::string(LogTag) + ": no Engine writer for \"" + proposal.stickTrack.title +
                             "\"; its rating and comment were not written");
            return {};
        }
        // writeAnnotation throws when the track has gone from the
        // database since the scan. The cues for this track are already
        // written by now, so failing the whole save over a rating would
        // undo more than it saves: log it and let the rest through, the
        // way the OneLibrary paths below do.
        // Reported as written only on the path that actually wrote. A
        // save whose every Engine write threw has put nothing into the
        // scratch copy, and saying otherwise would have a cancel commit
        // that untouched copy back over the stick -- reverting whatever
        // another change wrote to the real m.db.
        try {
            engine->writeAnnotation(sourceId, stars, comment);
            return {.ok = true, .wroteToWriteRoot = true};
        } catch (const std::exception &e) {
            ctx.log().record(std::string(LogTag) + ": Engine annotation write failed for \"" +
                             proposal.stickTrack.title + "\": " + e.what());
        }
        return {};
    }

    if (format == "rekordbox") {
        AnnotationOutcome outcome;
        // The rating is a one-byte field in export.pdb, which no cue
        // write touches, so it needs a writer of its own.
        //
        // Written and committed here rather than buffered across the
        // save. PdbRowWriter reads the whole file at construction and
        // commit() refuses if anything changed underneath, so holding
        // one open across a save that another change also writes
        // export.pdb into guarantees one of them loses.
        if (stars) {
          try {
            // The session's root, not the stick's: when another change
            // has this database redirected to a scratch copy, that copy
            // is the one that will be committed back.
            const std::string pdbPath =
                (fs::path(writer.session.writeRoot()) / "rekordbox" / "export.pdb").string();
            infrastructure::rekordbox::PdbRowWriter rows(pdbPath);
            if (!rows.setTrackRating(static_cast<uint32_t>(std::stoul(sourceId)), *stars)) {
                // The row this proposal was built from is not in the
                // catalog any more. Nothing to write and nothing broken,
                // so the save carries on and the log says why.
                ctx.log().record(std::string(LogTag) + ": no DeviceLibrary row with id=" + sourceId +
                                 " (\"" + proposal.stickTrack.title + "\"); its rating was not written");
            } else if (!rows.commit()) {
                ctx.log().record(std::string(LogTag) + ": FAILED to write the restored rating into " + pdbPath +
                                 " for \"" + proposal.stickTrack.title + "\"");
                outcome.ok = false;
            } else {
                // Into the session's write root -- the scratch copy of
                // export.pdb when this save has one. It has to be
                // counted for exactly the reason the Engine path is:
                // FormatWriteSession discards a scratch nothing was
                // applied to, so an uncounted rating is a rating a
                // cancelled or failed save throws away after the page
                // has already been told it landed.
                outcome.wroteToWriteRoot = true;
            }
          } catch (const std::exception &e) {
            // A missing, truncated or unparseable export.pdb, or a
            // rating out of range. The rating did not reach the catalog
            // either way, so this reports a failed save for the same
            // reason the commit() branch above does: success here would
            // take the track off the page's list with the old rating
            // still on the stick. Same room, other door.
            ctx.log().record(std::string(LogTag) + ": DeviceLibrary rating write failed for \"" +
                             proposal.stickTrack.title + "\": " + e.what());
            outcome.ok = false;
          }
        }
        // The mirror is the other half of the same library, and it is
        // the half that can take a comment of any length. Attempted even
        // when export.pdb refused: a separate database, the only half
        // that takes a comment at all, and giving up a write that had
        // nothing wrong with it helps nobody.
        bool mirrorTookIt = false;
        if (writer.mirror && !proposal.stickTrack.filePath.empty()) {
            try {
                writer.mirror->writeAnnotationForPath(proposal.stickTrack.filePath, stars, comment);
                mirrorTookIt = true;
                // Deliberately NOT counted: this is exportLibrary.db,
                // a different database from the one this session holds.
            } catch (const std::exception &e) {
                ctx.log().record(std::string(LogTag) + ": OneLibrary annotation write failed: " + e.what());
            }
        }
        if (comment && !mirrorTookIt) {
            // Measured: export.pdb keeps a comment in a byte span fixed
            // at export time, and 1160 of the fixture's 1161 tracks have
            // a span of zero. Writing a truncated prefix of the DJ's own
            // sentence would be worse than not writing it, so this says
            // so instead. The page says the same thing before the save.
            //
            // Only when the mirror did not take it: on a stick carrying
            // both catalogs the comment did go back, and saying it could
            // not would contradict both the page and the truth.
            ctx.log().record(std::string(LogTag) + ": DeviceLibrary cannot store a comment for \"" +
                             proposal.stickTrack.title + "\" -- its row has no room to grow one");
        }
        return outcome;
    }

    // format == "onelibrary": the adapter is this format's cue writer,
    // and it holds the one OneLibrary connection this save opens. Going
    // through it rather than constructing a second writer is what keeps
    // the whole save at one key derivation instead of one per track.
    auto *adapter = dynamic_cast<OneLibraryCueWriterAdapter *>(writer.writer.get());
    if (!adapter) {
        ctx.log().record(std::string(LogTag) + ": no OneLibrary writer for \"" + proposal.stickTrack.title +
                         "\"; its rating and comment were not written");
        return {};
    }
    try {
        adapter->writer().writeAnnotationForPath(proposal.stickTrack.filePath, stars, comment);
        return {.ok = true, .wroteToWriteRoot = true};
    } catch (const std::exception &e) {
        ctx.log().record(std::string(LogTag) + ": OneLibrary annotation write failed: " + e.what());
    }
    return {};
}

}  // namespace

RestoreMetadataChange::RestoreMetadataChange(QString format, QString path, QString sourceId,
                                              domain::MetadataRestoreProposal proposal, int itemCountHint)
    : m_format(std::move(format)), m_path(std::move(path)), m_sourceId(std::move(sourceId)),
      m_proposal(std::move(proposal)), m_itemCountHint(itemCountHint)
{
}

QString RestoreMetadataChange::id() const
{
    return "metadata-restore:" + m_format + ":" + m_sourceId;
}

QString RestoreMetadataChange::description() const
{
    const QString title = QString::fromStdString(m_proposal.stickTrack.title.empty()
                                                      ? m_proposal.stickTrack.filename
                                                      : m_proposal.stickTrack.title);
    const int cues = static_cast<int>(m_proposal.cues.size());
    // A proposal can be staged for its rating or its comment alone --
    // the cues may conflict and be skipped, or there may be none stored.
    // Saying "Put 0 stored cue(s) back" would describe a write this
    // change no longer makes.
    if (!m_proposal.cuesOffered) {
        QStringList fields;
        if (m_proposal.ratingOffered) {
            fields << QStringLiteral("rating");
        }
        if (m_proposal.commentOffered) {
            fields << QStringLiteral("comment");
        }
        return QStringLiteral("Put the stored %1 back on \"%2\"").arg(fields.join(QStringLiteral(" and ")), title);
    }
    if (m_proposal.cuesFillAGap) {
        return QStringLiteral("Put %1 stored cue(s) back on \"%2\"").arg(cues).arg(title);
    }
    return QStringLiteral("Replace the cues on \"%1\" with the %2 stored one(s)").arg(title).arg(cues);
}

QString RestoreMetadataChange::verb() const
{
    return QStringLiteral("restored");
}

QString RestoreMetadataChange::unit() const
{
    return QStringLiteral("tracks");
}

QStringList RestoreMetadataChange::formatsTouched() const
{
    return {m_format};
}

// The format decides which catalog is written; rekordbox additionally
// writes this track's own analysis file, whose path needs the shared
// index apply() would build anyway. Same shape as MergeCuesChange.
//
// Both flags follow what this particular proposal will actually write,
// because WriteScope is declared in both directions: a proposal staged
// for its rating alone writes no analysis file, and one that restores a
// rating does rewrite export.pdb -- which the up-front backup has to
// cover rather than leaving to applyAnnotation's backupOnce mid-loop.
std::vector<BackupTarget> RestoreMetadataChange::filesToBackup(SaveContext &ctx) const
{
    std::vector<BackupTarget> targets;
    const domain::TrackId track{m_format.toStdString(), m_sourceId.toStdString()};
    const WriteScope scope{.cueData = m_proposal.cuesOffered,
                           .catalogRows = m_proposal.ratingOffered,
                           .oneLibraryMirror = true};
    for (const auto &file : filesWrittenFor(scope, track, m_path, ctx)) {
        targets.push_back({file, LogTag});
    }
    return targets;
}

ChangeOutcome RestoreMetadataChange::apply(SaveContext &ctx)
{
    const domain::Track &track = m_proposal.stickTrack;
    const std::string sourceId = m_sourceId.toStdString();

    // One writer per format for the whole save, not one per track. The
    // key deliberately does not carry the sourceId: the OneLibrary
    // adapter learns this track's path through notePath() below, so
    // every track in the save shares one adapter and therefore one
    // SQLCipher connection and one key derivation. Keying it per track
    // cost two opens per track, ~115 ms of PBKDF2 each -- see
    // tests/restore_metadata_change_test.cpp case 6, which counts them.
    const std::string key = "metadata-restore:" + m_format.toStdString();
    RestoreWriterContext &writer = ctx.shared<RestoreWriterContext>(
        key, [&]() { return std::make_unique<RestoreWriterContext>(m_format, m_path, ctx, m_itemCountHint); });
    if (auto *adapter = dynamic_cast<OneLibraryCueWriterAdapter *>(writer.writer.get())) {
        adapter->notePath(sourceId, track.filePath);
    }

    if (m_format == "rekordbox" && m_proposal.cuesOffered) {
        // rekordbox keeps cues per track, in ANLZ files, so the file to
        // back up is this track's own. Nothing to back up when no cue
        // write is coming: the rating goes into export.pdb, which
        // applyAnnotation backs up itself.
        const std::string root = m_path.toStdString();
        const auto *pathIndex = sharedAnlzPathIndex(ctx, m_path);
        const auto trackId = static_cast<uint32_t>(std::stoul(sourceId));
        auto analyzePath = pathIndex ? pathIndex->pathFor(trackId)
                                     : infrastructure::rekordbox::findAnlzPathForTrackId(root, trackId);
        if (analyzePath) {
            ctx.backupOnce(infrastructure::rekordbox::extAnlzPath(root, *analyzePath), LogTag);
        }
    }

    // Only when the plan actually offers cues. Every writer treats the
    // vector it is handed as the complete set for the track, so writing
    // an empty one does not mean "leave the cues alone" -- it means
    // "delete them". A proposal staged for its rating alone carries no
    // cues, and under the skip-all default a track whose cues conflict
    // carries none either: exactly the DJ's live work that the default
    // exists to protect.
    bool wroteIntoWriteRoot = false;
    if (m_proposal.cuesOffered) {
        writer.writer->writeHotCues(sourceId, m_proposal.cues);
        // Counted, because FormatWriteSession throws a scratch copy away
        // on a cancel when nothing was applied to it. Engine and
        // OneLibrary cues are written INTO that copy, so leaving them
        // uncounted meant a cancelled save could discard the copy
        // holding them while the summary still counted the tracks as
        // restored. rekordbox cues are exempt only because they live in
        // ANLZ files the session does not manage.
        wroteIntoWriteRoot = m_format != "rekordbox";
        ctx.log().record(std::string(LogTag) + ": wrote " + std::to_string(m_proposal.cues.size()) +
                         " stored cue(s) onto " + m_format.toStdString() + " track id=" + sourceId + " (\"" +
                         track.title + "\")");

        if (writer.mirror && !track.filePath.empty()) {
            try {
                writer.mirror->writeCuesForPath(track.filePath, m_proposal.cues);
                ctx.log().record(std::string(LogTag) + ": also wrote them into OneLibrary (id=" + sourceId + ")");
            } catch (const std::exception &e) {
                ctx.log().record(std::string(LogTag) + ": OneLibrary cue write failed for \"" + track.title +
                                 "\": " + e.what());
            }
        }
    }

    const AnnotationOutcome annotation = applyAnnotation(ctx, writer, m_format, m_path, sourceId, m_proposal);
    // Every format, rekordbox included: the rating goes into
    // session.writeRoot()/rekordbox/export.pdb, which IS the scratch
    // copy when this save has one. Only the cue write above is exempt
    // for rekordbox, and only because ANLZ files are not the session's.
    if (annotation.wroteToWriteRoot) {
        wroteIntoWriteRoot = true;
    }

    // Once per track, not once per write. FormatWriteSession throws a
    // scratch copy away on a cancel when nothing was applied to it, so
    // this has to be counted -- Engine and OneLibrary writes go INTO
    // that copy -- but the same count is the "N update(s)" the commit
    // log reports, and a track that got both cues and a rating is still
    // one track. rekordbox's CUES are the only exemption: they live in
    // per-track ANLZ files the session does not manage. Its rating is
    // not exempt -- that goes into the session's own export.pdb.
    if (wroteIntoWriteRoot) {
        writer.session.noteItemApplied();
    }

    if (!annotation.ok) {
        return ChangeOutcome::failure("The restored rating could not be written into export.pdb for \"" +
                                      QString::fromStdString(track.title) + "\".");
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
