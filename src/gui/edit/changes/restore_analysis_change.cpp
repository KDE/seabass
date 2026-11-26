// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/restore_analysis_change.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

#include "application/path_key.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "application/ports/analysis_writer.hpp"
#include "domain/cue_tolerance.hpp"
#include "gui/edit/changes/analysis_ledger_record.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/edit/changes/kept_analysis_save.hpp"
#include "gui/edit/changes/reanalyse_track_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;
using infrastructure::rekordbox::AnlzFile;

constexpr const char *Label = "track-reanalysis";
constexpr std::uint32_t Ppth = 0x50505448;  // "PPTH"

std::string exportPdbUnder(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

bool isFile(const std::string &path)
{
    std::error_code ec;
    return fs::is_regular_file(pathFromUtf8(path), ec);
}

std::optional<std::string> fileBytes(const std::string &path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<std::string> sectionOf(const AnlzFile &file, std::uint32_t fourcc)
{
    for (const auto &section : file.sections) {
        if (section.fourcc == fourcc) {
            return section.rawBytes;
        }
    }
    return std::nullopt;
}

// The same cue points in any order, within a millisecond.
bool sameCueSet(const std::vector<domain::CuePoint> &a, const std::vector<domain::CuePoint> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    std::vector<bool> taken(b.size(), false);
    for (const domain::CuePoint &cue : a) {
        bool found = false;
        for (std::size_t i = 0; i < b.size() && !found; ++i) {
            const domain::CuePoint &other = b[i];
            if (!taken[i] && other.kind == cue.kind && other.hotCueNumber == cue.hotCueNumber
                && other.isLoop == cue.isLoop && std::abs(other.positionMs - cue.positionMs) < 1.0
                && (!cue.isLoop || std::abs(other.loopEndMs - cue.loopEndMs) < 1.0)) {
                taken[i] = found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// What this save's restores left out, for the summary, counted only for
// the changes that landed.
struct RestoreNotes
{
    std::map<QString, std::string> refused;
    std::map<QString, std::string> cuesLeft;
};

RestoreNotes &saveNotesOf(SaveContext &ctx)
{
    bool created = false;
    auto &notes = ctx.sharedForWholeSave<RestoreNotes>("restore-analysis:notes", [&]() {
        created = true;
        return std::make_unique<RestoreNotes>();
    });
    if (created) {
        ctx.onAfterCommit([&ctx](const SaveContext::AfterCommit &after) {
            const auto *notes = ctx.sharedForWholeSaveIfPresent<RestoreNotes>("restore-analysis:notes");
            if (notes == nullptr) {
                return;
            }
            int refused = 0;
            for (const auto &[id, why] : notes->refused) {
                refused += after.skippedIds.contains(id) ? 1 : 0;
            }
            if (refused > 0) {
                ctx.addSummaryNote(
                    refused == 1
                        ? QStringLiteral("One track kept the analysis it has: it changed since Seabass analysed it, or "
                                         "what was kept no longer belongs to it (the log says why).")
                        : QStringLiteral("%1 tracks kept the analysis they have: they changed since Seabass analysed "
                                         "them, or what was kept no longer belongs to them (the log says why).")
                              .arg(refused));
            }
            int left = 0;
            for (const auto &[id, why] : notes->cuesLeft) {
                left += after.appliedIds.contains(id) && !after.skippedIds.contains(id) ? 1 : 0;
            }
            if (left > 0) {
                ctx.addSummaryNote(left == 1
                                       ? QStringLiteral("The cue points of one restored track were left where they "
                                                        "are: they changed since the review, or the libraries hold "
                                                        "different ones.")
                                       : QStringLiteral("The cue points of %1 restored tracks were left where they "
                                                        "are: they changed since the review, or the libraries hold "
                                                        "different ones.")
                                             .arg(left));
            }
        });
    }
    return notes;
}

// One row's cue points as the save reads them.
struct RowCues
{
    std::string sourceId;
    std::vector<domain::CuePoint> cues;
    bool unreadable = false;
};

// One analysis file to put back: the kept bytes, or none (a file Analyse
// again made, which goes again).
struct FileBack
{
    std::string path;
    std::optional<AnlzFile> current;
    std::optional<std::string> bytes;
};

}  // namespace

RestoreAnalysisChange::RestoreAnalysisChange(std::string audioFile, std::string stickPath, QString stickRoot,
                                             QString pioneerRoot, QString enginePath,
                                             std::vector<std::string> keptDatPaths, bool engineKept, QString title,
                                             QString artist, int itemCountHint)
    : m_audioFile(std::move(audioFile)), m_stickPath(std::move(stickPath)), m_stickRoot(std::move(stickRoot)),
      m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)),
      m_keptDatPaths(std::move(keptDatPaths)), m_engineKept(engineKept), m_title(std::move(title)),
      m_artist(std::move(artist)), m_itemCountHint(itemCountHint)
{
    if (m_audioFile.empty() || m_stickRoot.isEmpty() || m_stickPath.empty() || m_stickPath.front() != '/') {
        throw std::invalid_argument("RestoreAnalysisChange: a file on a stick is required");
    }
    for (const std::string &dat : m_keptDatPaths) {
        if (m_pioneerRoot.isEmpty() || dat.rfind("/PIONEER/", 0) != 0) {
            throw std::invalid_argument("RestoreAnalysisChange: \"" + dat + "\" is no analysis file on this stick");
        }
    }
    if (m_keptDatPaths.empty() && !m_engineKept) {
        throw std::invalid_argument("RestoreAnalysisChange: nothing is kept to put back");
    }
}

QString RestoreAnalysisChange::idFor(const std::string &stickPath)
{
    return QStringLiteral("addcue:restore-analysis:") + QString::fromStdString(application::normalizedPathKey(stickPath));
}

QString RestoreAnalysisChange::id() const
{
    return idFor(m_stickPath);
}

QString RestoreAnalysisChange::owner() const
{
    return QStringLiteral("addcue");
}

QString RestoreAnalysisChange::description() const
{
    return QStringLiteral("Restore the original analysis of \"%1\"").arg(subject());
}

QString RestoreAnalysisChange::subject() const
{
    const QString title = m_title.isEmpty() ? pathToQString(pathFromUtf8(m_audioFile).filename()) : m_title;
    return m_artist.isEmpty() ? title : title + QStringLiteral(", ") + m_artist;
}

QString RestoreAnalysisChange::unit() const
{
    return QStringLiteral("tracks");
}

QString RestoreAnalysisChange::verb() const
{
    return QStringLiteral("given back their original analysis");
}

QStringList RestoreAnalysisChange::unitKeys() const
{
    return {id(), ReanalyseTrackChange::unitKeyFor(m_stickPath)};
}

QStringList RestoreAnalysisChange::formatsTouched() const
{
    QStringList formats;
    if (!m_keptDatPaths.empty()) {
        formats << QStringLiteral("rekordbox") << QStringLiteral("onelibrary");
    }
    if (!m_enginePath.isEmpty()) {
        formats << QStringLiteral("engine");
    }
    return formats;
}

std::vector<std::string> RestoreAnalysisChange::anlzFilesOf(const std::string &datPath) const
{
    const fs::path root = pathFromUtf8(m_stickRoot.toStdString());
    const std::string stem = datPath.substr(1, datPath.size() - 4);
    std::vector<std::string> files;
    for (const char *extension : {"DAT", "EXT", "2EX"}) {
        files.push_back(pathToUtf8(root / pathFromUtf8(stem + extension)));
    }
    return files;
}

std::string RestoreAnalysisChange::ledgerStickRoot() const
{
    return infrastructure::paths::normalStickRootForCatalogPath(
        (m_pioneerRoot.isEmpty() ? m_enginePath : m_pioneerRoot).toStdString());
}

std::string RestoreAnalysisChange::ledgerKey() const
{
    const std::string relative = application::stickRelativePathOf(m_audioFile, ledgerStickRoot());
    return relative.empty() ? std::string() : application::normalizedPathKey(relative);
}

std::vector<BackupTarget> RestoreAnalysisChange::filesToBackup(SaveContext &) const
{
    std::vector<BackupTarget> targets;
    const std::string stickRoot = ledgerStickRoot();
    const std::string pioneer = m_pioneerRoot.toStdString();
    if (!m_keptDatPaths.empty()) {
        if (const std::string kept = keptRekordboxAnalysisFileOf(stickRoot, ledgerKey()); !kept.empty()) {
            targets.push_back({kept, Label, true});
        }
        for (const std::string &dat : m_keptDatPaths) {
            for (const std::string &anlz : anlzFilesOf(dat)) {
                targets.push_back({anlz, Label, true});
            }
        }
        if (isFile(exportPdbUnder(pioneer))) {
            targets.push_back({exportPdbUnder(pioneer), Label});
        }
        if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
            targets.push_back({infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label});
        }
    }
    if (!m_enginePath.isEmpty()) {
        targets.push_back({FormatWriteSession::databaseFileFor("engine", m_enginePath.toStdString()), Label});
        if (const std::string kept = keptEngineAnalysisFileOf(stickRoot, ledgerKey()); !kept.empty()) {
            targets.push_back({kept, Label, true});
        }
        targets.push_back({beatGridLedgerFileOf(stickRoot), Label, true});
    }
    targets.push_back(analysisLedgerBackupTarget(stickRoot, Label));
    return targets;
}

std::vector<RekordboxWrite> RestoreAnalysisChange::rekordboxWrites() const
{
    if (!m_written) {
        return {};
    }
    return {*m_written};
}

ChangeOutcome RestoreAnalysisChange::apply(SaveContext &ctx)
{
    using KeptStatus = infrastructure::local::KeptRekordboxAnalysisLookup::Status;
    m_written.reset();
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string name = "\"" + subject().toStdString() + "\" (" + m_stickPath + ")";
    const std::string stickRoot = ledgerStickRoot();
    const std::string key = ledgerKey();
    std::vector<std::string> done;
    const auto refuse = [&](const std::string &why) {
        ctx.log().record(std::string(Label) + ": " + name + ": the original analysis not restored, nothing written: "
                         + why);
        saveNotesOf(ctx).refused[id()] = why;
        return ChangeOutcome::skip();
    };
    if (key.empty()) {
        return refuse("the file has no place under the stick's root");
    }
    auto &catalogs = LibraryCatalogCache::instance();

    try {
        // rekordbox's side: what is kept, checked against the stick now.
        std::optional<infrastructure::local::KeptRekordboxAnalysis> kept;
        std::vector<FileBack> filesBack;
        std::vector<std::uint32_t> pdbIds;
        std::vector<RowCues> deviceLibraryRows;
        std::vector<RowCues> oneLibraryRows;
        std::optional<infrastructure::rekordbox::PdbRowWriter> pdbRows;
        FormatWriteSession *rekordboxSession = nullptr;
        if (!m_keptDatPaths.empty()) {
            auto &store = saveKeptRekordboxStore(ctx, stickRoot);
            const auto found = store.find(key);
            if (found.status == KeptStatus::Damaged) {
                return refuse("what Seabass kept of rekordbox's analysis cannot be read (" + found.error + ")");
            }
            if (found.status == KeptStatus::None) {
                return refuse("nothing of rekordbox's analysis is kept for it any more");
            }
            kept = found.entry;
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(pathFromUtf8(m_audioFile), ec);
            if (ec || size != kept->audioFileSize) {
                return refuse(ec ? std::string("its audio file is not on the stick")
                                 : "its audio file changed since its analysis was kept (another size), so the kept "
                                   "analysis is of another file");
            }
            std::set<std::string> keptDats;
            for (const auto &set : kept->files) {
                keptDats.insert(application::normalizedPathKey(set.datPath));
            }
            std::set<std::string> declared;
            for (const std::string &dat : m_keptDatPaths) {
                declared.insert(application::normalizedPathKey(dat));
            }
            if (keptDats != declared) {
                return refuse("what is kept changed since the restore was staged");
            }
            // Every row's analysis file now has to be one that was kept.
            const auto notKept = [&](const std::string &datPath) {
                return keptDats.count(application::normalizedPathKey(datPath)) == 0;
            };
            if (isFile(exportPdbUnder(pioneer))) {
                rekordboxSession = &sharedFormatWriteSession(ctx, "rekordbox", pioneer, m_itemCountHint, Label);
                pdbRows.emplace(exportPdbUnder(rekordboxSession->writeRoot()));
                pdbIds = pdbRows->trackIdsWithFilePath(m_stickPath);
                const auto *index = sharedAnlzPathIndex(ctx, m_pioneerRoot);
                const auto source = infrastructure::rekordbox::anlzSourceForPioneerRoot(pioneer);
                for (const std::uint32_t row : pdbIds) {
                    const std::optional<std::string> analyzePath =
                        index ? index->pathFor(row) : infrastructure::rekordbox::findAnlzPathForTrackId(pioneer, row);
                    if (!analyzePath || analyzePath->empty()) {
                        continue;
                    }
                    if (notKept(*analyzePath)) {
                        return refuse("its rows name other analysis files now (" + *analyzePath
                                      + ") than the ones kept");
                    }
                    RowCues cues{std::to_string(row), {}, false};
                    try {
                        cues.cues = infrastructure::rekordbox::readAnalysisFileCues(*source, *analyzePath)
                                        .value_or(std::vector<domain::CuePoint>{});
                    } catch (const infrastructure::rekordbox::AnalysisFileUnreadable &) {
                        cues.unreadable = true;
                    }
                    deviceLibraryRows.push_back(std::move(cues));
                }
            }
            if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
                auto &writer = sharedOneLibraryWriter(ctx, pioneer);
                std::vector<std::int64_t> ids;
                try {
                    ids = writer.contentIdsForFile(m_audioFile);
                } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                }
                if (!ids.empty()) {
                    for (const std::string &file : writer.cueFilesForPath(m_audioFile)) {
                        const fs::path p = pathFromUtf8(file);
                        if (p.extension() == ".DAT" || p.extension() == ".dat") {
                            const std::string relative =
                                "/" + pathToGenericUtf8(fs::relative(p, pathFromUtf8(m_stickRoot.toStdString())));
                            if (notKept(relative)) {
                                return refuse("its OneLibrary rows name other analysis files now (" + relative
                                              + ") than the ones kept");
                            }
                        }
                    }
                }
                for (const std::int64_t row : ids) {
                    RowCues cues{std::to_string(row), {}, false};
                    try {
                        cues.cues = writer.analysisFileCuesOf(row).value_or(std::vector<domain::CuePoint>{});
                    } catch (const std::exception &) {
                        cues.unreadable = true;
                    }
                    oneLibraryRows.push_back(std::move(cues));
                }
            }
            // The grid there now has to be Seabass's or the kept one.
            std::string ledgerGrid;
            if (const auto *ledger = saveAnalysisLedger(ctx, stickRoot)) {
                if (const auto entry = ledger->find(key); entry != ledger->end()) {
                    ledgerGrid = entry->second.rekordboxGrid;
                }
            }
            for (const auto &set : kept->files) {
                const std::vector<std::string> paths = anlzFilesOf(set.datPath);
                const std::optional<std::string> *originals[] = {&set.dat, &set.ext, &set.twoEx};
                for (std::size_t i = 0; i < paths.size(); ++i) {
                    FileBack back{paths[i], std::nullopt, *originals[i]};
                    if (isFile(paths[i])) {
                        back.current = AnlzFile::readRaw(paths[i]);
                    }
                    filesBack.push_back(std::move(back));
                }
                const FileBack &dat = filesBack[filesBack.size() - 3];
                if (!dat.current || !set.dat) {
                    return refuse("its analysis file " + set.datPath + " is not on the stick");
                }
                const AnlzFile original = AnlzFile::fromBytes(*set.dat, "the kept " + set.datPath);
                if (sectionOf(*dat.current, Ppth) != sectionOf(original, Ppth)) {
                    return refuse(set.datPath + " names another audio file now than when it was kept");
                }
                const std::string gridNow = infrastructure::rekordbox::pqtzGridFingerprint(*dat.current);
                const bool seabass = !gridNow.empty()
                                     && (gridNow == ledgerGrid
                                         || std::find(kept->seabassGrids.begin(), kept->seabassGrids.end(), gridNow)
                                                != kept->seabassGrids.end());
                if (!seabass && gridNow != kept->originalGrid) {
                    return refuse("its beat grid in " + set.datPath
                                  + " was changed since Seabass analysed it (in rekordbox or on a player), and Seabass "
                                    "does not write over that");
                }
            }
        }

        // Engine's side: the writer's own preview of putting back what it
        // kept, for each row of the file.
        FormatWriteSession *engineSession = nullptr;
        std::vector<std::int64_t> engineRestores;
        struct EngineCues
        {
            std::int64_t id = 0;
            std::vector<domain::CuePoint> cues;
        };
        std::vector<EngineCues> engineRows;
        const bool hasEngine = !m_enginePath.isEmpty()
                               && isFile(FormatWriteSession::databaseFileFor("engine", m_enginePath.toStdString()));
        if (hasEngine) {
            engineSession = &sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, Label);
            auto &writer = engineAnalysisWriterFor(ctx, *engineSession, stickRoot);
            infrastructure::engine::LibdjinteropEngineReader reader(engineSession->writeRoot());
            reader.setAudioRoot(engineSession->realRoot());
            std::string why;
            for (const std::int64_t row :
                 infrastructure::engine::engineTrackIdsForFile(engineSession->writeRoot(), engineSession->realRoot(),
                                                               m_audioFile)) {
                using Outcome = application::EngineAnalysisRestoreResult::Outcome;
                const auto preview = writer.restoreEngineAnalysis(std::to_string(row), key, {false, true});
                if (preview.outcome == Outcome::Restored || preview.outcome == Outcome::AlreadyEngines) {
                    engineRestores.push_back(row);
                } else if (preview.outcome == Outcome::ChangedSince) {
                    return refuse("Engine's row " + std::to_string(row) + ": " + preview.reason);
                } else if (preview.outcome == Outcome::KeptDamaged) {
                    return refuse("what Seabass kept of the player's analysis cannot be read (" + preview.reason + ")");
                } else if (preview.outcome == Outcome::Stale && why.empty()) {
                    why = preview.reason;
                }
                engineRows.push_back({row, reader.readTrack(std::to_string(row)).value_or(domain::Track{}).cues});
            }
            if (m_engineKept && engineRestores.empty() && !kept) {
                return refuse(why.empty() ? std::string("nothing of the player's analysis is kept for it any more")
                                          : "what is kept of the player's analysis does not belong to its row: " + why);
            }
        }
        if (!kept && engineRestores.empty()) {
            return refuse("nothing is kept for it any more");
        }

        // The cues, planned before anything is written: all or nothing.
        const int cueSwitch = m_cueSwitch.load();
        bool cuesMove = false;
        std::vector<std::pair<std::string, std::vector<domain::CuePoint>>> deviceLibraryCues;
        std::optional<std::vector<domain::CuePoint>> oneLibraryCues;
        std::vector<std::pair<std::int64_t, std::vector<domain::CuePoint>>> engineCues;
        if (kept && isOn(cueSwitch) && m_cueReview.plan.verdict == application::CueGridPlan::Verdict::Moves) {
            const double tolerance = domain::cueToleranceMsFor(m_cueReview.oldBpm, m_cueReview.oldBpm);
            std::string whyNot;
            const auto no = [&](const std::string &why) {
                if (whyNot.empty()) {
                    whyNot = why;
                }
            };
            const auto planFor = [&](const std::string &library, const RowCues &row)
                -> std::optional<std::vector<domain::CuePoint>> {
                if (row.unreadable) {
                    no(library + "'s cue points could not be read");
                    return std::nullopt;
                }
                if (row.cues.empty()) {
                    return std::nullopt;
                }
                const auto plan = application::planCueGridMoves(row.cues, m_cueReview.oldBeats, m_cueReview.newBeats);
                if (!ReanalyseTrackChange::cuesMatchTheReview(m_cueReview.plan, plan, tolerance)) {
                    no(library + " holds other cue points than the review showed");
                    return std::nullopt;
                }
                if (plan.moves.empty()) {
                    return std::nullopt;
                }
                return application::cuesMovedOntoNewGrid(row.cues, plan);
            };
            for (const auto *rows : {&deviceLibraryRows, &oneLibraryRows}) {
                for (const RowCues &row : *rows) {
                    if (!sameCueSet(row.cues, rows->front().cues)) {
                        no("its rows hold different cue points");
                    }
                }
            }
            if (!deviceLibraryRows.empty() && !oneLibraryRows.empty()
                && !sameCueSet(deviceLibraryRows.front().cues, oneLibraryRows.front().cues)) {
                no("DeviceLibrary and OneLibrary hold different cue points");
            }
            for (const RowCues &row : deviceLibraryRows) {
                if (auto moved = planFor("DeviceLibrary", row)) {
                    deviceLibraryCues.emplace_back(row.sourceId, std::move(*moved));
                }
            }
            for (const RowCues &row : oneLibraryRows) {
                if (auto moved = planFor("OneLibrary", row)) {
                    oneLibraryCues = std::move(*moved);
                }
            }
            for (const EngineCues &row : engineRows) {
                if (row.cues.empty()) {
                    continue;
                }
                if (std::find(engineRestores.begin(), engineRestores.end(), row.id) == engineRestores.end()) {
                    no("Engine keeps the grid it has (nothing of the player's analysis is kept for it)");
                    continue;
                }
                auto moved = ReanalyseTrackChange::engineCuesOntoNewGrid(m_cueReview.plan, row.cues,
                                                                         m_cueReview.oldBeats, m_cueReview.newBeats,
                                                                         tolerance);
                if (!moved) {
                    no("Engine holds other cue points than the review showed");
                } else if (!sameCueSet(*moved, row.cues)) {
                    engineCues.emplace_back(row.id, std::move(*moved));
                }
            }
            cuesMove = whyNot.empty() && (!deviceLibraryCues.empty() || oneLibraryCues || !engineCues.empty());
            if (!cuesMove) {
                const std::string why = whyNot.empty() ? "no library holds the cue points the review showed" : whyNot;
                ctx.log().record(std::string(Label) + ": " + name
                                 + ": cue points left where they are, though moving them back was asked for: " + why);
                saveNotesOf(ctx).cuesLeft[id()] = why;
            }
        }

        // The writes. rekordbox's files first, each read back.
        bool rekordboxRestored = false;
        if (kept) {
            for (FileBack &back : filesBack) {
                if (!back.bytes) {
                    if (back.current) {
                        std::string failure;
                        if (!infrastructure::removeEntry(pathFromUtf8(back.path), failure)) {
                            return ChangeOutcome::failure(QStringLiteral("Could not take away %1: %2")
                                                              .arg(QString::fromStdString(back.path),
                                                                   QString::fromStdString(failure)));
                        }
                        infrastructure::fsyncDirectoryContaining(back.path);
                        done.push_back("took away " + pathToUtf8(pathFromUtf8(back.path).filename())
                                       + ", which Analyse again made");
                    }
                    continue;
                }
                const AnlzFile original = AnlzFile::fromBytes(*back.bytes, "the kept " + back.path);
                std::string intended;
                if (back.current) {
                    const AnlzFile restored = infrastructure::rekordbox::restoreAnlzAnalysis(original, *back.current);
                    intended = restored.toBytes();
                    if (intended != back.current->toBytes()) {
                        restored.writeRaw(back.path);
                    }
                } else {
                    intended = *back.bytes;
                    if (!infrastructure::writeFileDurablyAtomic(back.path, intended)) {
                        return ChangeOutcome::failure(
                            QStringLiteral("Could not write the analysis file %1.").arg(QString::fromStdString(back.path)));
                    }
                }
                if (fileBytes(back.path) != intended) {
                    return ChangeOutcome::failure(QStringLiteral("The analysis file %1 does not read back as it was "
                                                                 "written.")
                                                      .arg(QString::fromStdString(back.path)));
                }
            }
            catalogs.invalidateWithOneLibraryMirror("rekordbox", pioneer);
            done.push_back("rekordbox's analysis files put back");
            rekordboxRestored = true;

            // The rows' tempo and key, as they were kept, on the rows of
            // the file that are still there.
            if (pdbRows) {
                int rows = 0;
                for (const auto &row : kept->rows) {
                    if (row.library != "rekordbox" || !row.tempo || !row.keyId
                        || std::find(pdbIds.begin(), pdbIds.end(), static_cast<std::uint32_t>(row.rowId)) == pdbIds.end()) {
                        continue;
                    }
                    infrastructure::rekordbox::PdbRowWriter::TrackTempoAndKeyId values{
                        static_cast<std::uint32_t>(*row.tempo), static_cast<std::uint32_t>(*row.keyId)};
                    rows += pdbRows->setTrackTempoAndKeyId(static_cast<std::uint32_t>(row.rowId), values) ? 1 : 0;
                }
                if (rows > 0) {
                    if (!pdbRows->commit()) {
                        return ChangeOutcome::failure(QStringLiteral("Could not write export.pdb for %1.").arg(subject()));
                    }
                    const infrastructure::rekordbox::PdbRowWriter back(exportPdbUnder(rekordboxSession->writeRoot()));
                    for (const auto &row : kept->rows) {
                        if (row.library != "rekordbox" || !row.tempo || !row.keyId
                            || std::find(pdbIds.begin(), pdbIds.end(), static_cast<std::uint32_t>(row.rowId))
                                   == pdbIds.end()) {
                            continue;
                        }
                        const auto now = back.trackTempoAndKeyId(static_cast<std::uint32_t>(row.rowId));
                        if (!now || now->tempo != *row.tempo || now->keyId != *row.keyId) {
                            return ChangeOutcome::failure(
                                QStringLiteral("export.pdb does not read back the tempo and key put back for %1.")
                                    .arg(subject()));
                        }
                    }
                    rekordboxSession->noteItemApplied();
                    catalogs.invalidate("rekordbox", pioneer);
                    done.push_back("export.pdb: tempo and key of " + std::to_string(rows) + " row(s) put back");
                }
            }
            if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
                std::vector<infrastructure::onelibrary::OneLibraryCueWriter::ContentTempoAndKey> rows;
                for (const auto &row : kept->rows) {
                    if (row.library == "onelibrary") {
                        rows.push_back({row.rowId, row.tempo, row.keyId});
                    }
                }
                if (!rows.empty()) {
                    ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
                    const int written = sharedOneLibraryWriter(ctx, pioneer).setTempoAndKeyIds(rows);
                    catalogs.invalidateWithOneLibraryMirror("onelibrary", pioneer);
                    done.push_back("exportLibrary.db: tempo and key of " + std::to_string(written) + " row(s) put back");
                }
            }
        }

        // Engine's rows, by the writer, which checks again and drops what
        // it kept once it is back.
        bool engineRestored = false;
        if (!engineRestores.empty()) {
            auto &writer = engineAnalysisWriterFor(ctx, *engineSession, stickRoot);
            for (const std::int64_t row : engineRestores) {
                using Outcome = application::EngineAnalysisRestoreResult::Outcome;
                const auto result = writer.restoreEngineAnalysis(std::to_string(row), key, {});
                if (result.outcome != Outcome::Restored && result.outcome != Outcome::AlreadyEngines) {
                    return ChangeOutcome::failure(QStringLiteral("Engine's row %1 for %2 could not be given its "
                                                                 "original analysis back: %3")
                                                      .arg(row)
                                                      .arg(subject(), QString::fromStdString(result.reason)));
                }
                engineRestored = true;
                done.push_back("Engine id " + std::to_string(row) + ": the player's analysis put back");
            }
            forgetGridInLedgerAfterCommit(ctx, stickRoot, id(), key);
            engineSession->noteItemApplied();
            catalogs.invalidate("engine", m_enginePath.toStdString());
        }

        // The cues back onto the original grid, last.
        if (cuesMove) {
            if (const int under = std::max(cueSwitch, 0); under > 0) {
                done.push_back("its cue points moved back on their own, every move under " + std::to_string(under)
                               + " ms");
            }
            if (!deviceLibraryCues.empty()) {
                infrastructure::rekordbox::RekordboxCueWriter writer(pioneer, sharedAnlzPathIndex(ctx, m_pioneerRoot));
                for (const auto &[row, moved] : deviceLibraryCues) {
                    writer.writeHotCues(row, moved);
                }
                if (!oneLibraryRows.empty()) {
                    const QString failed =
                        mirrorCuesOrExplain(sharedOneLibraryWriter(ctx, pioneer), m_audioFile,
                                            deviceLibraryCues.front().second, ctx, Label,
                                            QStringLiteral("move the cue points back"));
                    if (!failed.isEmpty()) {
                        return ChangeOutcome::failure(failed);
                    }
                }
                m_written = RekordboxWrite{m_audioFile, deviceLibraryCues.front().second, std::nullopt};
                done.push_back("rekordbox: cue points moved back onto the original grid");
            } else if (oneLibraryCues) {
                sharedOneLibraryWriter(ctx, pioneer).writeCuesForPath(m_audioFile, *oneLibraryCues);
                m_written = RekordboxWrite{m_audioFile, *oneLibraryCues, std::nullopt};
                done.push_back("OneLibrary: cue points moved back onto the original grid");
            }
            catalogs.invalidateWithOneLibraryMirror("rekordbox", pioneer);
            catalogs.invalidateWithOneLibraryMirror("onelibrary", pioneer);
            if (!engineCues.empty()) {
                auto &writer = engineAnalysisWriterFor(ctx, *engineSession, stickRoot);
                for (const auto &[row, moved] : engineCues) {
                    writer.writeHotCues(std::to_string(row), moved);
                }
                catalogs.invalidate("engine", m_enginePath.toStdString());
                done.push_back("Engine: cue points moved back onto the player's grid");
            }
        }

        // What was kept has done its work: taken away in the save, so Undo
        // puts it back with the analysis it describes.
        if (rekordboxRestored) {
            saveKeptRekordboxStore(ctx, stickRoot).drop(key);
        }
        // The analysis ledger after the commit: no entry when no library
        // keeps Seabass's analysis of the track, else the entry without
        // the grid of the library that got its own back.
        const auto *ledger = saveAnalysisLedger(ctx, stickRoot);
        const auto entry = ledger ? ledger->find(key) : infrastructure::local::AnalysisLedger::const_iterator{};
        if (ledger && entry != ledger->end()) {
            infrastructure::local::AnalysisLedgerEntry next = entry->second;
            if (rekordboxRestored) {
                next.rekordboxGrid.clear();
            }
            if (engineRestored) {
                next.engineGrid.clear();
            }
            const bool engineStillSeabass = !next.engineGrid.empty() && !engineRows.empty();
            const bool rekordboxStillSeabass = !next.rekordboxGrid.empty() && !m_pioneerRoot.isEmpty();
            if (!engineStillSeabass && !rekordboxStillSeabass) {
                forgetAnalysisAfterCommit(ctx, stickRoot, id(), key, Label);
            } else {
                recordAnalysisAfterCommit(ctx, stickRoot, id(), next, Label);
            }
        } else {
            forgetAnalysisAfterCommit(ctx, stickRoot, id(), key, Label);
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }

    std::string line;
    for (const std::string &part : done) {
        line += (line.empty() ? "" : "; ") + part;
    }
    ctx.log().record(std::string(Label) + ": restored the original analysis of " + name + ": " + line);
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
