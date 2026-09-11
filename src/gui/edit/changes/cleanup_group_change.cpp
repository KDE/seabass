#include "infrastructure/paths/seabass_paths.hpp"
#include "gui/edit/changes/cleanup_group_change.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "application/ports/cue_writer.hpp"
#include "application/ports/library_cleanup_writer.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/onelibrary_cue_writer_adapter.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "infrastructure/engine/libdjinterop_engine_cleanup_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/stick_layout.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cleanup_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

// One format's writers for a clean-up. Which files a cleanup overwrites is
// answered by filesWrittenFor() in change_helpers -- one definition, so
// what the save declares and what it backs up cannot disagree.
struct CleanupFormatContext
{
    std::unique_ptr<application::CueWriter> cueWriter;
    std::unique_ptr<application::LibraryCleanupWriter> cleanupWriter;
    // Non-empty only for rekordbox, used to best-effort also write
    // cues into OneLibrary/exportLibrary.db if it exists alongside
    // export.pdb on this stick.
    std::string pioneerRoot;
};

// oneLibrarySourceIdToPath is only consulted when format == "onelibrary"
// (the adapters' own comments explain why sourceId alone isn't enough for
// this format).
//
// writeRoot: where the format's *shared database* writers (cleanupWriter
// always; cueWriter too for engine/onelibrary, whose cue writes hit that
// same shared file) should actually read/write, when the save staged it
// to fast local scratch first (FormatWriteSession) -- defaults to `path`
// itself, i.e. write straight to the stick. rekordbox's cueWriter is
// deliberately NOT redirected: it writes small per-track .ANLZ files,
// not the shared export.pdb, so staging buys it nothing (same reasoning
// as SyncPlanChange's rekordbox branch).
CleanupFormatContext makeContext(const QString &format, const QString &path,
                                 const std::unordered_map<std::string, std::string> &oneLibrarySourceIdToPath = {},
                                 std::optional<std::string> writeRoot = std::nullopt)
{
    CleanupFormatContext ctx;
    if (format == "rekordbox") {
        std::string pioneerRoot = path.toStdString();
        ctx.cueWriter = std::make_unique<infrastructure::rekordbox::RekordboxCueWriter>(pioneerRoot);
        ctx.cleanupWriter =
            std::make_unique<infrastructure::rekordbox::RekordboxCleanupWriter>(writeRoot.value_or(pioneerRoot));
        ctx.pioneerRoot = pioneerRoot;
    } else if (format == "engine") {
        std::string engineLibraryPath = path.toStdString();
        std::string effectivePath = writeRoot.value_or(engineLibraryPath);
        ctx.cueWriter = std::make_unique<infrastructure::engine::LibdjinteropEngineCueWriter>(effectivePath);
        ctx.cleanupWriter = std::make_unique<infrastructure::engine::LibdjinteropEngineCleanupWriter>(effectivePath);
        std::string engineDbFile = (fs::path(engineLibraryPath) / "Database2" / "m.db").string();
    } else {
        // onelibrary. `path` here is the PIONEER root, same as the
        // rekordbox branch -- OneLibrary lives alongside export.pdb.
        // realStickRoot is passed explicitly (not left to the adapters'
        // own pioneerRoot-parent default) because writeRoot may be a
        // scratch copy, whose parent is just a temp directory, not the
        // stick -- content.path lookups need the *real* stick root
        // regardless of where exportLibrary.db itself is being read from
        // right now.
        std::string pioneerRoot = path.toStdString();
        std::string effectivePath = writeRoot.value_or(pioneerRoot);
        std::string realStickRoot = fs::path(pioneerRoot).parent_path().string();
        ctx.cueWriter =
            std::make_unique<OneLibraryCueWriterAdapter>(effectivePath, oneLibrarySourceIdToPath, realStickRoot);
        ctx.cleanupWriter = std::make_unique<OneLibraryCleanupWriterAdapter>(effectivePath, oneLibrarySourceIdToPath,
                                                                             realStickRoot);
    }
    return ctx;
}

// The writers of one save's clean-ups for one format (SaveContext::
// shared): the database backed up once, a scratch copy for a big batch
// (FormatWriteSession), the format's writers on that write root, and the
// pending-deletion manifest every removed copy is appended to. OneLibrary's
// adapters are bound to the tracks they write, so that format gets one
// context per group.
struct CleanupWriterContext
{
    CleanupWriterContext(const QString &format, const QString &path, int itemCountHint, SaveContext &ctx,
                         const std::unordered_map<std::string, std::string> &oneLibrarySourceIdToPath)
        : session(sharedFormatWriteSession(ctx, format.toStdString(), path.toStdString(), itemCountHint,
                                            "duplicate-file-cleanup")),
          manifest(infrastructure::paths::stickPendingDeletions(fs::path(path.toStdString()).parent_path()).string())
    {
        std::optional<std::string> writeRoot;
        if (session.usesScratch()) {
            writeRoot = session.writeRoot();
        }
        context = makeContext(format, path, oneLibrarySourceIdToPath, writeRoot);
        effectiveRoot = session.writeRoot();
        realStickRootForOneLib = fs::path(path.toStdString()).parent_path().string();
        // Named in every pending-deletion entry, so the review page can
        // point at the backup that still holds the removed row.
        dbBackupId = ctx.backupIdOf(session.databaseFile());
    }

    // The save's session for this database, shared with every
    // other change that writes it -- see sharedFormatWriteSession().
    FormatWriteSession &session;
    infrastructure::cleanup::PendingDeletionManifest manifest;
    CleanupFormatContext context;
    std::string effectiveRoot;
    std::string realStickRootForOneLib;
    std::string dbBackupId;
};

}  // namespace

CleanupGroupChange::CleanupGroupChange(QString format, QString path, domain::DuplicateCleanupPlan plan,
                                        int itemCountHint)
    : m_format(std::move(format)), m_path(std::move(path)), m_plan(std::move(plan)), m_itemCountHint(itemCountHint)
{
}

QString CleanupGroupChange::id() const
{
    return "cleanup:" + m_format + ":" + QString::fromStdString(m_plan.survivor.sourceId);
}

QString CleanupGroupChange::description() const
{
    int newCues =
        static_cast<int>(m_plan.mergedCuesForSurvivor.size()) - static_cast<int>(m_plan.survivor.cues.size());
    return QStringLiteral("Clean up \"%1\": keep %2, remove %3 cop%4%5")
        .arg(QString::fromStdString(m_plan.survivor.title), QString::fromStdString(m_plan.survivor.filename))
        .arg(m_plan.toRemove.size())
        .arg(m_plan.toRemove.size() == 1 ? "y" : "ies")
        .arg(newCues > 0 ? QStringLiteral(" (%1 cue(s) preserved)").arg(newCues) : QString());
}

QString CleanupGroupChange::verb() const
{
    return QStringLiteral("cleaned up");
}

QString CleanupGroupChange::unit() const
{
    return QStringLiteral("groups");
}

QStringList CleanupGroupChange::doomedRowFormats() const
{
    QStringList formats;
    for (const auto &format : domain::catalogsWrittenBy(m_plan)) {
        formats << QString::fromStdString(format);
    }
    return formats;
}

// Every format this group would have to write to, not just the one the
// page is showing. On an uncollapsed plan that is exactly m_format, since
// each row is its own track; a collapsed file carries the other formats'
// rows in catalogRows and they have to be declared, or the save would
// take a lock on one catalog and write another.
QStringList CleanupGroupChange::formatsTouched() const
{
    QStringList formats{m_format};
    for (const auto &format : doomedRowFormats()) {
        if (!formats.contains(format)) {
            formats << format;
        }
    }
    return formats;
}

namespace
{

// A stray file has no catalog row to remove and no sourceId a writer
// would recognise -- its sourceId is a file path -- so it never reaches a
// cleanup writer. It gets a manifest line routing it to "Delete Orphaned
// Files" and nothing else. unreferencedFilesHeldBack gets no line at all:
// the planner refused those, and this is not the place to second-guess it.
void recordStrayFilesForDeletion(infrastructure::cleanup::PendingDeletionManifest &manifest,
                                 const domain::DuplicateCleanupPlan &plan, const std::string &format,
                                 application::OperationLog &log)
{
    for (const auto &stray : plan.unreferencedFilesToDelete) {
        infrastructure::cleanup::PendingDeletion pending;
        pending.format = format;
        pending.filePath = stray.filePath;
        pending.title = stray.title;
        pending.artist = stray.artist;
        manifest.append(pending);
        log.record("cleanup: no catalog references \"" + stray.filePath
                   + "\"; recorded for deletion, kept copy is id=" + plan.survivor.sourceId);
    }
}

// True when applying this plan would write nothing to any catalog: no row
// to remove, no cue to merge onto the survivor, no field to fill in.
//
// Not only the all-stray group. The commonest shape on a real stick is one
// catalogued row plus one stray copy of it: the catalogued row survives,
// so the plan's only removal is a file, and a stray carries no cues, bpm,
// key or artwork to propagate. Such a plan is a line in the
// pending-deletion manifest and nothing else -- opening a write session
// for it would back up the database, possibly copy the whole file to
// scratch and back, and change not one byte of it.
bool writesToCatalog(const domain::DuplicateCleanupPlan &plan)
{
    if (plan.mergedCuesForSurvivor.size() > plan.survivor.cues.size()) {
        return true;
    }
    if (plan.bpmForSurvivor || plan.keyForSurvivor || plan.artworkPathForSurvivor) {
        return true;
    }
    return std::any_of(plan.toRemove.begin(), plan.toRemove.end(),
                       [](const domain::Track &t) { return !t.isUnreferenced; });
}

}  // namespace

// A cleanup merges cues onto the survivor, then removes the doomed rows and
// repoints playlists -- catalog rows and the OneLibrary copy of them.
//
// The doomed tracks' own analysis files are deliberately absent: a cleanup
// removes their catalog rows, it does not write their cue files. Only the
// survivor's is written.
//
// A plan that writes to no catalog backs nothing up. It is a line in the
// pending-deletion manifest and nothing else, so declaring files for it
// would have Undo restore files this save never touched.
std::vector<BackupTarget> CleanupGroupChange::filesToBackup(SaveContext &ctx) const
{
    if (!writesToCatalog(m_plan)) {
        return {};
    }
    const WriteScope scope{.cueData = true, .catalogRows = true, .oneLibraryMirror = true};
    std::vector<BackupTarget> targets;
    auto declare = [&](const std::string &format, const std::string &survivorSourceId, const QString &root) {
        for (const auto &file : filesWrittenFor(scope, {format, survivorSourceId}, root, ctx)) {
            targets.push_back({file, "duplicate-file-cleanup"});
        }
    };

    declare(m_format.toStdString(), domain::rowIdIn(m_plan.survivor, m_format.toStdString()), m_path);

    // Every OTHER catalog this plan writes, declared here because a save
    // must back up everything it will overwrite before it overwrites any
    // of it -- and apply() below removes the doomed rows from each of
    // them. Keyed per format on purpose: filesWrittenFor takes a
    // (format, sourceId) pair because a sourceId is not unique across
    // catalogs, and this decides which file gets overwritten. Passing
    // m_format's survivor id while naming another catalog's root would
    // back up one file while the save overwrote a different one.
    //
    // A catalog whose path cannot be found on this stick is skipped
    // rather than guessed at: apply() refuses such a plan outright, so
    // nothing is written there and nothing needs restoring. Guessing a
    // path here is the failure filesWrittenFor's own comment warns about.
    for (const auto &format : domain::catalogsWrittenBy(m_plan)) {
        if (format == m_format.toStdString()) {
            continue;
        }
        const auto targets_ = domain::writeTargetsFor(m_plan, format);
        if (domain::hasNoWork(targets_) || targets_.survivorSourceId.empty()) {
            continue;
        }
        const std::string path = infrastructure::catalogPathFor(format, m_path.toStdString());
        if (path.empty()) {
            continue;
        }
        declare(format, targets_.survivorSourceId, QString::fromStdString(path));
    }
    return targets;
}

ChangeOutcome CleanupGroupChange::apply(SaveContext &ctx)
{
    const auto &plan = m_plan;

    // Decided for EVERY catalog before ANY of them is written. A collapsed
    // file carries one row per catalog and removing it means removing all
    // of them; a save that wrote the catalogs it could and then hit one it
    // could not would leave the library in exactly the split state this
    // feature exists to prevent. So the whole plan is checked first and
    // refused whole.
    //
    // Two ways a catalog is unwritable, and they are different failures:
    // the survivor has no row there to repoint the removed rows'
    // playlists at (canWriteWholeCatalog, the writer-side view of
    // DuplicateCleanupPlan::wouldStrandAFormat), or the catalog names a
    // file this stick does not have. The second is not hypothetical --
    // catalogPathFor() checks existence rather than deriving a path and
    // hoping, because handing a writer a path to a database that is not
    // there has it create one.
    std::vector<std::pair<std::string, QString>> catalogsToWrite;  // format -> its path
    for (const auto &format : domain::catalogsWrittenBy(plan)) {
        const auto targets = domain::writeTargetsFor(plan, format);
        if (!domain::canWriteWholeCatalog(targets)) {
            return ChangeOutcome::failure(
                QStringLiteral("\"%1\" has a %2 row to remove but no %2 row to keep, so removing it "
                                "would take the track out of that catalog entirely. Nothing was written.")
                    .arg(QString::fromStdString(plan.survivor.title), QString::fromStdString(format)));
        }
        if (domain::hasNoWork(targets)) {
            continue;
        }
        const QString path = format == m_format.toStdString()
            ? m_path
            : QString::fromStdString(infrastructure::catalogPathFor(format, m_path.toStdString()));
        if (path.isEmpty()) {
            return ChangeOutcome::failure(
                QStringLiteral("\"%1\" has a %2 row to remove, but this stick has no %2 catalog to "
                                "remove it from. Nothing was written.")
                    .arg(QString::fromStdString(plan.survivor.title), QString::fromStdString(format)));
        }
        catalogsToWrite.push_back({format, path});
    }

    // A plan that writes to no catalog is one or more manifest lines and
    // nothing else, so it deliberately opens no write session: the
    // manifest is append-per-call precisely so it needs none.
    if (!writesToCatalog(plan)) {
        infrastructure::cleanup::PendingDeletionManifest manifest(
            infrastructure::paths::stickPendingDeletions(fs::path(m_path.toStdString()).parent_path()).string());
        recordStrayFilesForDeletion(manifest, plan, m_format.toStdString(), ctx.log());
        return ChangeOutcome::success();
    }

    std::string key = "cleanup:" + m_format.toStdString();
    std::unordered_map<std::string, std::string> oneLibrarySourceIdToPath;
    if (m_format == "onelibrary") {
        // Keyed by the OneLibrary row id, the id the writes below use --
        // on a collapsed file that is not the representative row's id.
        oneLibrarySourceIdToPath[domain::rowIdIn(plan.survivor, "onelibrary")] = plan.survivor.filePath;
        for (const auto &doomed : plan.toRemove) {
            oneLibrarySourceIdToPath[domain::rowIdIn(doomed, "onelibrary")] = doomed.filePath;
        }
        key += ":" + plan.survivor.sourceId;
    }
    CleanupWriterContext &w = ctx.shared<CleanupWriterContext>(key, [&]() {
        return std::make_unique<CleanupWriterContext>(m_format, m_path, m_itemCountHint, ctx,
                                                      oneLibrarySourceIdToPath);
    });
    CleanupFormatContext &fc = w.context;

    // When the plan carries a OneLibrary row of its own, the loop at the
    // end of this function writes that catalog properly -- by row id,
    // with its own backup and session. The best-effort mirrors below
    // would then write it a SECOND time, by path, outside that session.
    // They exist for the uncollapsed case, where nothing else touches
    // OneLibrary at all and leaving its row behind orphans it. Once it is
    // a catalog in its own right, mirroring it is not a safety net, it is
    // a duplicate write to a file another session already owns.
    const bool oneLibraryWrittenAsCatalog =
        std::any_of(catalogsToWrite.begin(), catalogsToWrite.end(),
                    [](const std::pair<std::string, QString> &entry) { return entry.first == "onelibrary"; });
    application::OperationLog &log = ctx.log();
    const QString &format = m_format;

    // Row ids in the page's own format. A collapsed file's representative
    // row is whichever catalog was read first (rekordbox before Engine),
    // so plan.survivor.sourceId can be a rekordbox id while this change
    // writes Engine; ids are dense from 1 in both, and the wrong one
    // lands on an unrelated track. Every write below goes through this.
    const std::string primaryFormat = m_format.toStdString();
    auto idIn = [&](const std::string &baseSourceId) -> std::string {
        for (const auto &t : plan.group.tracks) {
            if (t.sourceId == baseSourceId) {
                return domain::rowIdIn(t, primaryFormat);
            }
        }
        return {};
    };
    const std::string survivorId = domain::rowIdIn(plan.survivor, primaryFormat);
    if (survivorId.empty()) {
        return ChangeOutcome::failure(QString("The kept copy has no %1 row to write to; rescan and try again.")
                                          .arg(m_format));
    }

    // Fallback for anything filesToBackup() did not declare, resolved the
    // same way so it cannot disagree with it -- and so it costs no second
    // export.pdb parse.
    for (const auto &f :
         filesWrittenFor({.cueData = true, .catalogRows = true, .oneLibraryMirror = true},
                         {primaryFormat, survivorId}, m_path, ctx)) {
        ctx.backupOnce(f, "duplicate-file-cleanup");
    }

    // sourceId -> filePath among this group's own tracks -- only
    // OneLibrary's propagateMissingFieldsForPath() below needs this
    // (it identifies tracks by path, not sourceId, same reason
    // OneLibraryCueWriter's class comment gives).
    auto findTrackFilePath = [&plan](const std::string &sourceId) -> std::string {
        for (const auto &t : plan.group.tracks) {
            if (t.sourceId == sourceId) {
                return t.filePath;
            }
        }
        return {};
    };

    if (plan.mergedCuesForSurvivor.size() > plan.survivor.cues.size()) {
        fc.cueWriter->writeHotCues(survivorId, plan.mergedCuesForSurvivor);
        w.session.noteItemApplied();
        log.record("cleanup: wrote merged cues onto survivor track id=" + survivorId);

        // Best-effort secondary write, alongside the primary write
        // above, never fatal to this operation. See OneLibraryCueWriter's
        // own class comment and docs/onelibrary-format.md.
        if (!fc.pioneerRoot.empty() && !plan.survivor.filePath.empty()
            && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(fc.pioneerRoot) && !oneLibraryWrittenAsCatalog) {
            try {
                sharedOneLibraryWriter(ctx, fc.pioneerRoot)
                    .writeCuesForPath(plan.survivor.filePath, plan.mergedCuesForSurvivor);
                log.record("cleanup: also wrote merged cues onto survivor into OneLibrary (id="
                           + plan.survivor.sourceId + ")");
            } catch (const std::exception &e) {
                log.record("cleanup: OneLibrary cue write failed for \"" + plan.survivor.title + "\": " + e.what());
            }
        }
    }

    // Fills in the survivor's missing bpm/key/artwork from whichever
    // other copy in the group has each (see domain::DuplicateCleanupPlan's
    // own comment on why this is a per-field "fill a gap", not a merge).
    // Primary-format writes here are NOT best-effort: a failure fails
    // this change. Only the OneLibrary *mirror* of a rekordbox write,
    // below, is best-effort, same as the merged-cues mirror.
    if (plan.bpmForSurvivor || plan.keyForSurvivor || plan.artworkPathForSurvivor) {
        if (format == "rekordbox") {
            std::string pdbPath = w.effectiveRoot + "/rekordbox/export.pdb";
            infrastructure::rekordbox::PdbRowWriter fieldWriter(pdbPath);
            uint32_t survivorRow = static_cast<uint32_t>(std::stoul(survivorId));
            if (plan.keyForSurvivor) {
                fieldWriter.copyTrackFieldsIfMissing(static_cast<uint32_t>(std::stoul(idIn(plan.keyDonorSourceId))),
                                                     survivorRow, true, false, false);
            }
            if (plan.bpmForSurvivor) {
                fieldWriter.copyTrackFieldsIfMissing(static_cast<uint32_t>(std::stoul(idIn(plan.bpmDonorSourceId))),
                                                     survivorRow, false, true, false);
            }
            if (plan.artworkPathForSurvivor) {
                fieldWriter.copyTrackFieldsIfMissing(static_cast<uint32_t>(std::stoul(idIn(plan.artworkDonorSourceId))),
                                                     survivorRow, false, false, true);
            }
            if (!fieldWriter.commit()) {
                return ChangeOutcome::failure("failed to write " + QString::fromStdString(pdbPath));
            }
            w.session.noteItemApplied();
            log.record("cleanup: propagated missing bpm/key/artwork onto survivor track id=" + survivorId);
        } else if (format == "engine") {
            // Artwork is deliberately not offered here -- Engine track
            // artwork isn't writable through libdjinterop today, see
            // propagateMissingFields()'s own doc comment. cueWriter is
            // always this concrete type for format == "engine".
            auto *engineCueWriter =
                static_cast<infrastructure::engine::LibdjinteropEngineCueWriter *>(fc.cueWriter.get());
            engineCueWriter->propagateMissingFields(survivorId, plan.bpmForSurvivor, plan.keyForSurvivor);
            w.session.noteItemApplied();
            log.record("cleanup: propagated missing bpm/key onto survivor track id=" + survivorId);
        } else if (format == "onelibrary") {
            auto &fieldWriter = sharedOneLibraryWriter(ctx, w.effectiveRoot, w.realStickRootForOneLib);
            if (plan.keyForSurvivor) {
                std::string donorPath = findTrackFilePath(plan.keyDonorSourceId);
                if (!donorPath.empty() && !plan.survivor.filePath.empty()) {
                    fieldWriter.propagateMissingFieldsForPath(donorPath, plan.survivor.filePath, false, true, false);
                }
            }
            if (plan.bpmForSurvivor) {
                std::string donorPath = findTrackFilePath(plan.bpmDonorSourceId);
                if (!donorPath.empty() && !plan.survivor.filePath.empty()) {
                    fieldWriter.propagateMissingFieldsForPath(donorPath, plan.survivor.filePath, true, false, false);
                }
            }
            if (plan.artworkPathForSurvivor) {
                std::string donorPath = findTrackFilePath(plan.artworkDonorSourceId);
                if (!donorPath.empty() && !plan.survivor.filePath.empty()) {
                    fieldWriter.propagateMissingFieldsForPath(donorPath, plan.survivor.filePath, false, false, true);
                }
            }
            w.session.noteItemApplied();
            log.record("cleanup: propagated missing bpm/key/artwork onto survivor track id=" + survivorId);
        }

        // Best-effort mirror onto OneLibrary too -- only reachable when
        // this format is rekordbox (format == "onelibrary" already wrote
        // OneLibrary directly above, as the primary write).
        if (format == "rekordbox" && !fc.pioneerRoot.empty() && !plan.survivor.filePath.empty()
            && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(fc.pioneerRoot) && !oneLibraryWrittenAsCatalog) {
            try {
                auto &oneLibFieldWriter = sharedOneLibraryWriter(ctx, fc.pioneerRoot);
                if (plan.keyForSurvivor) {
                    std::string donorPath = findTrackFilePath(plan.keyDonorSourceId);
                    if (!donorPath.empty()) {
                        oneLibFieldWriter.propagateMissingFieldsForPath(donorPath, plan.survivor.filePath, false, true,
                                                                        false);
                    }
                }
                if (plan.bpmForSurvivor) {
                    std::string donorPath = findTrackFilePath(plan.bpmDonorSourceId);
                    if (!donorPath.empty()) {
                        oneLibFieldWriter.propagateMissingFieldsForPath(donorPath, plan.survivor.filePath, true, false,
                                                                        false);
                    }
                }
                if (plan.artworkPathForSurvivor) {
                    std::string donorPath = findTrackFilePath(plan.artworkDonorSourceId);
                    if (!donorPath.empty()) {
                        oneLibFieldWriter.propagateMissingFieldsForPath(donorPath, plan.survivor.filePath, false, false,
                                                                        true);
                    }
                }
                log.record("cleanup: also propagated missing bpm/key/artwork into OneLibrary (id="
                           + plan.survivor.sourceId + ")");
            } catch (const std::exception &e) {
                log.record("cleanup: OneLibrary field propagation failed for \"" + plan.survivor.title
                           + "\": " + e.what());
            }
        }
    }

    for (const auto &doomed : plan.toRemove) {
        if (doomed.isUnreferenced) {
            // No catalog row to remove and no sourceId a writer would
            // recognise -- its sourceId is a file path. Handled after this
            // loop, by recordStrayFilesForDeletion().
            continue;
        }
        const std::string doomedId = domain::rowIdIn(doomed, primaryFormat);
        if (doomedId.empty()) {
            // This copy has no row in the page's catalog (it was read from
            // another one); its own catalog's removal happens in the
            // per-catalog loop below.
            continue;
        }
        fc.cleanupWriter->removeTrackReplacingWith(doomedId, survivorId);
        w.session.noteItemApplied();
        log.record("cleanup: removed duplicate track id=" + doomedId + " (\"" + doomed.title
                   + "\"), replaced by survivor id=" + survivorId);

        // Best-effort OneLibrary mirror. Without this, the doomed
        // track's own OneLibrary row is left pointing at a file this
        // change is about to schedule for deletion, becoming an orphan
        // (this is exactly how real orphaned rows were found on
        // production data, see docs/onelibrary-format.md).
        if (!fc.pioneerRoot.empty() && !doomed.filePath.empty() && !plan.survivor.filePath.empty()
            && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(fc.pioneerRoot) && !oneLibraryWrittenAsCatalog) {
            try {
                // Reassigns the doomed row's OneLibrary playlist
                // memberships onto the survivor instead of dropping
                // them -- see removeTrackByPathReplacingWith()'s comment.
                sharedOneLibraryWriter(ctx, fc.pioneerRoot)
                    .removeTrackByPathReplacingWith(doomed.filePath, plan.survivor.filePath);
                log.record("cleanup: also removed OneLibrary row for id=" + doomed.sourceId);
            } catch (const std::exception &e) {
                log.record("cleanup: OneLibrary row removal failed for \"" + doomed.title + "\": " + e.what());
            }
        }

        // On-stick state, appended per doomed copy: whatever this save
        // gets through has its manifest line, cancelled or not.
        infrastructure::cleanup::PendingDeletion pending;
        pending.format = format.toStdString();
        pending.filePath = doomed.filePath;
        pending.title = doomed.title;
        pending.artist = doomed.artist;
        pending.backupId = w.dbBackupId;
        w.manifest.append(pending);
    }

    // Every OTHER catalog listing this file. Cue merge FIRST, then
    // removal, per catalog: removing a row drops the cues stored on it,
    // so a catalog whose survivor row was never given the merged set
    // loses whatever only the doomed row held. That is the whole reason
    // this cannot be one write repeated -- each catalog has its own
    // survivor row, its own doomed rows, and its own cues to preserve.
    //
    // ctx.shared() is already keyed, so a per-format context sits beside
    // the primary one in the same save: one FormatWriteSession per
    // catalog, each with its own backup, exactly as SyncPlanChange does
    // it. Reached only for a collapsed plan; until collapse is switched
    // on in the scan, catalogsToWrite holds m_format alone and this loop
    // does not run.
    for (const auto &[secondaryFormat, secondaryPath] : catalogsToWrite) {
        if (secondaryFormat == m_format.toStdString()) {
            continue;
        }
        const auto targets = domain::writeTargetsFor(plan, secondaryFormat);
        const QString qFormat = QString::fromStdString(secondaryFormat);

        std::unordered_map<std::string, std::string> secondaryIdToPath;
        std::string secondaryKey = "cleanup:" + secondaryFormat;
        if (secondaryFormat == "onelibrary") {
            secondaryIdToPath[targets.survivorSourceId] = plan.survivor.filePath;
            for (const auto &doomed : plan.toRemove) {
                // The same id the removal below is issued with; keyed by
                // the base sourceId this map never matched on a
                // collapsed file.
                secondaryIdToPath[domain::rowIdIn(doomed, "onelibrary")] = doomed.filePath;
            }
            secondaryKey += ":" + targets.survivorSourceId;
        }
        CleanupWriterContext &sw = ctx.shared<CleanupWriterContext>(secondaryKey, [&]() {
            return std::make_unique<CleanupWriterContext>(qFormat, secondaryPath, m_itemCountHint, ctx,
                                                          secondaryIdToPath);
        });

        // Same fallback the primary path uses, and for the same reason:
        // whatever filesToBackup() already declared is deduplicated by
        // backupOnce(), and anything it could not is covered here before
        // this catalog is touched.
        for (const auto &f :
             filesWrittenFor({.cueData = true, .catalogRows = true, .oneLibraryMirror = true},
                             {secondaryFormat, targets.survivorSourceId}, secondaryPath, ctx)) {
            ctx.backupOnce(f, "duplicate-file-cleanup");
        }

        if (plan.mergedCuesForSurvivor.size() > plan.survivor.cues.size()) {
            sw.context.cueWriter->writeHotCues(targets.survivorSourceId, plan.mergedCuesForSurvivor);
            sw.session.noteItemApplied();
            log.record("cleanup: wrote merged cues onto the " + secondaryFormat + " survivor row id="
                       + targets.survivorSourceId);
        }

        for (const auto &doomedId : targets.doomedSourceIds) {
            sw.context.cleanupWriter->removeTrackReplacingWith(doomedId, targets.survivorSourceId);
            sw.session.noteItemApplied();
            log.record("cleanup: removed the " + secondaryFormat + " row id=" + doomedId
                       + ", replaced by survivor id=" + targets.survivorSourceId);
        }
    }

    // One manifest line per doomed FILE, not per catalog row: the file is
    // deleted once however many catalogs listed it, and the primary pass
    // above has already written those lines.
    recordStrayFilesForDeletion(w.manifest, plan, format.toStdString(), log);
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
