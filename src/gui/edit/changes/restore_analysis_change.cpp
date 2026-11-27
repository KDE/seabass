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
#include "infrastructure/local/kept_engine_analysis_file.hpp"
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
    // The restored tracks whose Engine row keeps the grid it has while
    // rekordbox's goes back: what went back, what stayed and why, and
    // where the cue points are, one line per track.
    std::map<QString, QString> libraries;
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
            for (const auto &[id, text] : notes->libraries) {
                if (after.appliedIds.contains(id) && !after.skippedIds.contains(id)) {
                    ctx.addSummaryNote(text);
                }
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

namespace
{

// Whether `cue` is where `move` put it: the same kind, pad and loop-ness,
// its place and a loop's end within the tolerance; how far off, or nullopt.
std::optional<double> offFromWhereMoved(const infrastructure::local::KeptCueMove &move, const domain::CuePoint &cue)
{
    constexpr double within = application::CueMoveMinimumMs;
    if ((cue.kind == domain::CuePoint::Kind::Hot) != move.hot || (move.hot && cue.hotCueNumber != move.pad)
        || cue.isLoop != move.loop) {
        return std::nullopt;
    }
    const double start = std::abs(cue.positionMs - move.newMs);
    const double end = move.loop ? std::abs(cue.loopEndMs - move.newEndMs) : 0.0;
    if (start >= within || end >= within) {
        return std::nullopt;
    }
    return start + end;
}

// Each recorded move of `library` with the cue it is made on (index into
// `cues`), as cueMovesBack() describes it.
std::vector<std::pair<const infrastructure::local::KeptCueMove *, std::size_t>>
movesBackOnto(const std::vector<infrastructure::local::KeptCueMove> &moves, const std::string &library,
              const std::vector<domain::CuePoint> &cues, int *blocked)
{
    struct Candidate
    {
        double off;
        std::size_t move;
        std::size_t cue;
    };
    std::vector<Candidate> candidates;
    for (std::size_t m = 0; m < moves.size(); ++m) {
        if (moves[m].library != library) {
            continue;
        }
        for (std::size_t c = 0; c < cues.size(); ++c) {
            if (const auto off = offFromWhereMoved(moves[m], cues[c])) {
                candidates.push_back({*off, m, c});
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) { return a.off < b.off; });
    std::vector<bool> moveTaken(moves.size(), false);
    std::vector<bool> cueTaken(cues.size(), false);
    std::vector<std::pair<const infrastructure::local::KeptCueMove *, std::size_t>> pairs;
    for (const Candidate &c : candidates) {
        if (!moveTaken[c.move] && !cueTaken[c.cue]) {
            moveTaken[c.move] = cueTaken[c.cue] = true;
            pairs.emplace_back(&moves[c.move], c.cue);
        }
    }
    // A cue does not go back onto another of its kind: each move checked
    // against the cues as they would be after every other move, until no
    // move is left that lands on one.
    std::vector<double> after(cues.size());
    for (std::size_t c = 0; c < cues.size(); ++c) {
        after[c] = cues[c].positionMs;
    }
    for (const auto &[move, cue] : pairs) {
        after[cue] = move->oldMs;
    }
    for (bool dropped = true; dropped;) {
        dropped = false;
        for (auto it = pairs.begin(); it != pairs.end(); ++it) {
            const auto [move, cue] = *it;
            bool onAnother = false;
            for (std::size_t other = 0; other < cues.size() && !onAnother; ++other) {
                onAnother = other != cue && cues[other].kind == cues[cue].kind && cues[other].isLoop == cues[cue].isLoop
                            && std::abs(after[other] - move->oldMs) < application::CueMoveMinimumMs;
            }
            if (onAnother) {
                after[cue] = cues[cue].positionMs;
                pairs.erase(it);
                if (blocked) {
                    ++*blocked;
                }
                dropped = true;
                break;
            }
        }
    }
    return pairs;
}

}  // namespace

application::CueGridPlan cueMovesBack(const std::vector<infrastructure::local::KeptCueMove> &moves,
                                      const std::string &library, const std::vector<domain::CuePoint> &cues,
                                      int *blocked)
{
    application::CueGridPlan plan;
    const auto planned = [&](const infrastructure::local::KeptCueMove &move, std::size_t at) {
        application::CueGridMove back;
        back.cueIndex = at;
        back.kind = move.hot ? domain::CuePoint::Kind::Hot : domain::CuePoint::Kind::Memory;
        back.hotCueNumber = move.pad;
        back.isLoop = move.loop;
        back.oldMs = move.newMs;
        back.newMs = move.oldMs;
        back.oldEndMs = move.newEndMs;
        back.newEndMs = move.oldEndMs;
        plan.moves.push_back(back);
    };
    if (cues.empty()) {
        for (const auto &move : moves) {
            if (move.library == library) {
                planned(move, 0);
            }
        }
    } else {
        for (const auto &[move, at] : movesBackOnto(moves, library, cues, blocked)) {
            planned(*move, at);
        }
    }
    plan.verdict = plan.moves.empty() ? application::CueGridPlan::Verdict::NothingMoves
                                      : application::CueGridPlan::Verdict::Moves;
    return plan;
}

std::optional<std::vector<domain::CuePoint>> cuesMovedBack(const std::vector<infrastructure::local::KeptCueMove> &moves,
                                                           const std::string &library,
                                                           const std::vector<domain::CuePoint> &cues, int *blocked)
{
    std::vector<domain::CuePoint> back = cues;
    const auto pairs = movesBackOnto(moves, library, cues, blocked);
    for (const auto &[move, at] : pairs) {
        back[at].positionMs = move->oldMs;
        if (move->loop) {
            back[at].loopEndMs = move->oldEndMs;
        }
    }
    return pairs.empty() ? std::nullopt : std::optional<std::vector<domain::CuePoint>>(std::move(back));
}

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
        for (const std::string &kept :
             {keptRekordboxAnalysisFileOf(stickRoot, ledgerKey()), keptRekordboxRecordFileOf(stickRoot, ledgerKey())}) {
            if (!kept.empty()) {
                targets.push_back({kept, Label, true});
            }
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
    if (auto ledger = analysisLedgerBackupTarget(stickRoot, Label)) {
        targets.push_back(*ledger);
    }
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
        infrastructure::local::KeptRekordboxRecord record;
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
            const auto recorded = store.findRecord(key);
            using RecordStatus = infrastructure::local::KeptRekordboxRecordLookup::Status;
            if (recorded.status != RecordStatus::Found) {
                return refuse("Seabass cannot tell whether the beat grid on the stick is its own: the record of the "
                              "grids it wrote, kept beside the original, "
                              + (recorded.status == RecordStatus::None ? std::string("is missing")
                                                                       : "cannot be read (" + recorded.error + ")"));
            }
            record = recorded.record;
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
                // Each file against its own kept original.
                const std::string gridNow = infrastructure::rekordbox::pqtzGridFingerprint(*dat.current);
                const bool seabass = isSeabassRekordboxGrid(ctx, stickRoot, gridNow, &record);
                if (!seabass && gridNow != infrastructure::rekordbox::pqtzGridFingerprint(original)) {
                    return refuse("its beat grid in " + set.datPath
                                  + " is neither the original nor one Seabass wrote: it was changed outside Seabass "
                                    "since the original was kept, and Seabass does not write over that");
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
        // Only the player's analysis Analyse again kept goes back with
        // rekordbox's: the record names it, and the kept one is still it.
        // One Beat Grids & Waveforms kept stays that page's, and Engine
        // keeps the grid it has.
        bool engineHere = hasEngine;
        // Why Engine keeps the grid it has where rekordbox's goes back, for
        // the log and the summary; No where it goes back too, or has no row.
        enum class EngineStays { No, CopiedElsewhere, NothingKept, NotAnalyseAgains, CannotGoBack };
        EngineStays engineStays = EngineStays::No;
        std::string engineWhy;
        if (hasEngine && kept) {
            const auto engineKept = infrastructure::local::StickKeptEngineAnalysisStore(pathFromUtf8(stickRoot)).find(key);
            const bool keptByAnalyseAgain =
                !record.engineKept.empty() && engineKept.status == application::KeptEngineAnalysisLookup::Status::Found
                && keptEngineOriginalFingerprint(engineKept.entry) == record.engineKept;
            if (!keptByAnalyseAgain) {
                engineHere = false;
                engineStays = engineKept.status == application::KeptEngineAnalysisLookup::Status::None
                                  ? EngineStays::NothingKept
                              : record.engineKept.empty() ? EngineStays::CopiedElsewhere
                                                          : EngineStays::NotAnalyseAgains;
            }
        }
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
                engineRows.push_back({row, reader.readTrack(std::to_string(row)).value_or(domain::Track{}).cues});
                if (!engineHere) {
                    continue;
                }
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
            }
            if (m_engineKept && engineRestores.empty() && !kept) {
                return refuse(why.empty() ? std::string("nothing of the player's analysis is kept for it any more")
                                          : "what is kept of the player's analysis does not belong to its row: " + why);
            }
            if (kept && engineHere && !engineRows.empty() && engineRestores.empty()) {
                engineStays = EngineStays::CannotGoBack;
                engineWhy = why;
            }
        }
        if (engineRows.empty()) {
            engineStays = EngineStays::No;
        }
        switch (engineStays) {
        case EngineStays::No:
            break;
        case EngineStays::NothingKept:
            done.push_back("Engine keeps the grid it has: nothing of the player's analysis was kept");
            break;
        case EngineStays::CopiedElsewhere:
            done.push_back("Engine keeps the grid it has: the player's analysis kept for it was kept by Copy Beat "
                           "Grids, and goes back on the Beat Grids & Waveforms page");
            break;
        case EngineStays::NotAnalyseAgains:
            done.push_back("Engine keeps the grid it has: the player's analysis kept for it is not the one Analyse "
                           "again kept");
            break;
        case EngineStays::CannotGoBack:
            done.push_back("Engine keeps the grid it has: the player's analysis Analyse again kept cannot go back to "
                           "its row"
                           + (engineWhy.empty() ? std::string() : " (" + engineWhy + ")")
                           + ", and it stays kept with rekordbox's");
            break;
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
        bool engineCuesStay = false;
        bool cuesBlocked = false;
        if (kept && isOn(cueSwitch) && !kept->cueMoves.empty()) {
            // Only the cues Analyse again moved (as the kept entry records
            // them), and only those still where it put them, go back to
            // where they were: no grid is planned, so a cue Analyse again
            // left where it was is never moved, and one set or changed
            // since stays. Each library on its own.
            std::vector<std::string> notes;
            bool rekordboxAgrees = true;
            const auto oneSet = [&](const std::vector<RowCues> &rows) {
                for (const RowCues &row : rows) {
                    rekordboxAgrees = rekordboxAgrees && !row.unreadable && sameCueSet(row.cues, rows.front().cues);
                }
            };
            oneSet(deviceLibraryRows);
            oneSet(oneLibraryRows);
            if (!deviceLibraryRows.empty() && !oneLibraryRows.empty()
                && !sameCueSet(deviceLibraryRows.front().cues, oneLibraryRows.front().cues)) {
                rekordboxAgrees = false;
            }
            const bool rekordboxMoved = std::any_of(kept->cueMoves.begin(), kept->cueMoves.end(),
                                                    [](const auto &move) { return move.library == "rekordbox"; });
            if (!rekordboxAgrees && rekordboxMoved) {
                notes.push_back("rekordbox's libraries hold different cue points (or cannot be read)");
            } else {
                for (const RowCues &row : deviceLibraryRows) {
                    if (auto moved = cuesMovedBack(kept->cueMoves, "rekordbox", row.cues)) {
                        deviceLibraryCues.emplace_back(row.sourceId, std::move(*moved));
                    }
                }
                if (deviceLibraryRows.empty()) {
                    for (const RowCues &row : oneLibraryRows) {
                        if (auto moved = cuesMovedBack(kept->cueMoves, "rekordbox", row.cues)) {
                            oneLibraryCues = std::move(*moved);
                        }
                    }
                }
            }
            // Cue points follow the grid of their own library: an Engine
            // row whose grid goes back gets its moved cues back with it;
            // one that keeps the grid it has keeps the cues Analyse again
            // moved onto that grid.
            for (const EngineCues &row : engineRows) {
                auto moved = cuesMovedBack(kept->cueMoves, "engine", row.cues);
                if (!moved) {
                    continue;
                }
                if (std::find(engineRestores.begin(), engineRestores.end(), row.id) == engineRestores.end()) {
                    engineCuesStay = true;
                    continue;
                }
                engineCues.emplace_back(row.id, std::move(*moved));
            }
            // All or nothing across the libraries whose grid goes back:
            // one that cannot take its cues back keeps every library's
            // where they are.
            if (!notes.empty()) {
                cuesBlocked = true;
                deviceLibraryCues.clear();
                oneLibraryCues.reset();
                engineCues.clear();
            }
            cuesMove = !deviceLibraryCues.empty() || oneLibraryCues || !engineCues.empty();
            if (!notes.empty() || (!cuesMove && !engineCuesStay)) {
                const std::string why = !notes.empty() ? notes.front() + ", so no cue point moves in any library"
                                                       : "none of the cue points Analyse again moved is where it put "
                                                         "them any more";
                ctx.log().record(std::string(Label) + ": " + name
                                 + ": cue points left where they are, though moving them back was asked for: " + why);
                saveNotesOf(ctx).cuesLeft[id()] = why;
            }
        }

        // The rows' tempo and key, planned and asked before the first
        // write. Every row that names the file now gets the kept values of
        // its library (the first kept row's, where the kept rows differ),
        // the key by its name: a kept key id is a number in the key table
        // as it was then, and a new export can give it to another key. A
        // library none of whose kept rows is one of its rows now (a new
        // export renumbered them) keeps its rows' tempo and key as they
        // are, and the log says so.
        struct RowsBack
        {
            std::vector<std::int64_t> ids;
            const infrastructure::local::KeptRekordboxRow *values = nullptr;
            int notKept = 0;
            std::string why;
        };
        const auto rowsBackOf = [&](const std::string &library, const std::vector<std::int64_t> &now) {
            RowsBack back;
            std::set<std::int64_t> keptIds;
            for (const auto &row : kept->rows) {
                if (row.library == library) {
                    keptIds.insert(row.rowId);
                    if (!back.values) {
                        back.values = &row;
                    }
                }
            }
            if (!back.values) {
                if (!now.empty()) {
                    back.why = "no row of it was kept, so their tempo and key stay as they are";
                }
                return back;
            }
            bool matched = false;
            for (const std::int64_t id : now) {
                matched = matched || keptIds.count(id) > 0;
                back.notKept += keptIds.count(id) == 0 ? 1 : 0;
            }
            if (!matched) {
                back.values = nullptr;
                back.notKept = 0;
                back.why = now.empty() ? "it has no row for the file any more"
                                       : "none of its rows for the file is one that was kept (a new export "
                                         "renumbered them), so their tempo and key stay as they are";
                return back;
            }
            back.ids = now;
            return back;
        };
        // The key of a kept row: none, by its name, or left as the row has
        // it where the kept id named no key (its name is not known).
        using PdbPutBack = infrastructure::rekordbox::PdbRowWriter::TrackTempoAndKeyPutBack;
        using OnePutBack = infrastructure::onelibrary::OneLibraryCueWriter::TempoAndKeyPutBack;
        std::optional<RowsBack> pdbBack;
        std::optional<RowsBack> oneBack;
        PdbPutBack pdbValues;
        OnePutBack oneValues;
        if (kept) {
            std::vector<std::int64_t> pdbNow(pdbIds.begin(), pdbIds.end());
            if (pdbRows) {
                pdbBack = rowsBackOf("rekordbox", pdbNow);
            }
            if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
                std::vector<std::int64_t> oneNow;
                for (const RowCues &row : oneLibraryRows) {
                    oneNow.push_back(std::stoll(row.sourceId));
                }
                oneBack = rowsBackOf("onelibrary", oneNow);
            }
            try {
                if (pdbBack && pdbBack->values) {
                    const auto &v = *pdbBack->values;
                    if (!v.tempo || *v.tempo < 0 || *v.tempo > 0xffffffffLL) {
                        return refuse("the tempo kept for its export.pdb rows is not one a row holds");
                    }
                    pdbValues.tempo = static_cast<std::uint32_t>(*v.tempo);
                    pdbValues.key = !v.keyId || *v.keyId == 0 ? PdbPutBack::Key::None
                                    : v.keyName              ? PdbPutBack::Key::Named
                                                             : PdbPutBack::Key::Leave;
                    pdbValues.keyName = v.keyName.value_or(std::string());
                    if (pdbValues.key == PdbPutBack::Key::Named && pdbValues.keyName.empty()) {
                        pdbValues.key = PdbPutBack::Key::None;
                    }
                    // In memory until the commit after the analysis files:
                    // a key row that cannot be placed refuses here.
                    for (const std::int64_t row : pdbBack->ids) {
                        pdbRows->putBackTrackTempoAndKey(static_cast<std::uint32_t>(row), pdbValues);
                    }
                }
                if (oneBack && oneBack->values) {
                    const auto &v = *oneBack->values;
                    oneValues.bpmx100 = v.tempo;
                    oneValues.key = !v.keyId || *v.keyId == 0 ? OnePutBack::Key::Raw
                                    : v.keyName              ? OnePutBack::Key::Named
                                                             : OnePutBack::Key::Leave;
                    oneValues.rawKeyId = oneValues.key == OnePutBack::Key::Raw ? v.keyId : std::nullopt;
                    oneValues.keyName = v.keyName.value_or(std::string());
                    if (oneValues.key == OnePutBack::Key::Named && oneValues.keyName.empty()) {
                        oneValues.key = OnePutBack::Key::Raw;
                        oneValues.rawKeyId.reset();
                    }
                    sharedOneLibraryWriter(ctx, pioneer).checkTempoAndKeyPutBack(oneValues);
                }
            } catch (const std::exception &e) {
                return refuse(std::string("the tempo and key kept for it cannot be put back (") + e.what() + ")");
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
            done.push_back(record.originKnown ? "rekordbox's analysis files put back"
                                              : "the analysis files that were on the stick before put back (who made "
                                                "them is not known)");
            rekordboxRestored = true;

            // The rows' tempo and key, as planned above.
            const auto keyWords = [](bool named, bool left, const std::string &name) {
                return named ? ", key " + name : left ? ", key left as it is (the key kept named no key)" : ", no key";
            };
            const auto notKeptWords = [](int n) {
                return n == 0 ? std::string()
                              : "; " + std::to_string(n) + " of them not there when the original was kept";
            };
            if (pdbBack && pdbBack->values) {
                if (!pdbRows->commit()) {
                    return ChangeOutcome::failure(QStringLiteral("Could not write export.pdb for %1.").arg(subject()));
                }
                const infrastructure::rekordbox::PdbRowWriter back(exportPdbUnder(rekordboxSession->writeRoot()));
                for (const std::int64_t row : pdbBack->ids) {
                    const auto now = back.trackTempoAndKeyId(static_cast<std::uint32_t>(row));
                    const bool keyLanded = pdbValues.key == PdbPutBack::Key::Leave
                                           || (pdbValues.key == PdbPutBack::Key::None ? now && now->keyId == 0
                                                                                      : now && now->keyName
                                                                                            && *now->keyName
                                                                                                   == pdbValues.keyName);
                    if (!now || now->tempo != pdbValues.tempo || !keyLanded) {
                        return ChangeOutcome::failure(
                            QStringLiteral("export.pdb does not read back the tempo and key put back for %1.")
                                .arg(subject()));
                    }
                }
                rekordboxSession->noteItemApplied();
                catalogs.invalidate("rekordbox", pioneer);
                done.push_back("export.pdb: tempo and key of " + std::to_string(pdbBack->ids.size()) + " row(s) put back"
                               + keyWords(pdbValues.key == PdbPutBack::Key::Named,
                                          pdbValues.key == PdbPutBack::Key::Leave, pdbValues.keyName)
                               + notKeptWords(pdbBack->notKept));
            } else if (pdbBack && !pdbBack->why.empty()) {
                done.push_back("export.pdb: " + pdbBack->why);
            }
            if (oneBack && oneBack->values) {
                ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
                const int written = sharedOneLibraryWriter(ctx, pioneer).putBackTempoAndKey(oneBack->ids, oneValues);
                catalogs.invalidateWithOneLibraryMirror("onelibrary", pioneer);
                done.push_back("exportLibrary.db: tempo and key of " + std::to_string(written) + " row(s) put back"
                               + keyWords(oneValues.key == OnePutBack::Key::Named,
                                          oneValues.key == OnePutBack::Key::Leave, oneValues.keyName)
                               + notKeptWords(oneBack->notKept));
            } else if (oneBack && !oneBack->why.empty()) {
                done.push_back("exportLibrary.db: " + oneBack->why);
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
                done.push_back("rekordbox: the cue points Analyse again moved put back where they were");
            } else if (oneLibraryCues) {
                sharedOneLibraryWriter(ctx, pioneer).writeCuesForPath(m_audioFile, *oneLibraryCues);
                m_written = RekordboxWrite{m_audioFile, *oneLibraryCues, std::nullopt};
                done.push_back("OneLibrary: the cue points Analyse again moved put back where they were");
            }
            catalogs.invalidateWithOneLibraryMirror("rekordbox", pioneer);
            catalogs.invalidateWithOneLibraryMirror("onelibrary", pioneer);
            if (!engineCues.empty()) {
                auto &writer = engineAnalysisWriterFor(ctx, *engineSession, stickRoot);
                for (const auto &[row, moved] : engineCues) {
                    writer.writeHotCues(std::to_string(row), moved);
                }
                catalogs.invalidate("engine", m_enginePath.toStdString());
                done.push_back("Engine: the cue points Analyse again moved put back where they were");
            }
        }

        // What was kept has done its work: taken away in the save, so Undo
        // puts it back with the analysis it describes. Only what went back
        // is done with: where the player's analysis Analyse again kept
        // could not go back with rekordbox's, the entry stays, its record
        // naming that analysis and its Engine cue moves, without the moves
        // of rekordbox's cues that went back.
        if (rekordboxRestored && engineStays == EngineStays::CannotGoBack) {
            infrastructure::local::KeptRekordboxAnalysis rest = *kept;
            std::erase_if(rest.cueMoves, [](const auto &move) { return move.library != "engine"; });
            if (!(rest == *kept)) {
                saveKeptRekordboxStore(ctx, stickRoot).keep(rest, record);
            }
            done.push_back("what is kept stays, for the player's analysis");
        } else if (rekordboxRestored) {
            saveKeptRekordboxStore(ctx, stickRoot).drop(key);
        }
        if (rekordboxRestored && engineStays != EngineStays::No) {
            QString text = QStringLiteral("\"%1\": rekordbox's libraries have the kept analysis back; Engine keeps "
                                          "the grid it has")
                               .arg(subject());
            switch (engineStays) {
            case EngineStays::CopiedElsewhere:
                text += QStringLiteral(", and the player's analysis that Copy Beat Grids kept goes back on the Beat "
                                       "Grids & Waveforms page, with Restore Engine's Analysis.");
                break;
            case EngineStays::NothingKept:
                text += QStringLiteral(", as nothing of the player's analysis was kept.");
                break;
            case EngineStays::NotAnalyseAgains:
                text += QStringLiteral(", as the player's analysis kept for it is not the one Analyse again kept.");
                break;
            case EngineStays::CannotGoBack:
                text += QStringLiteral(", as the player's analysis that Analyse again kept cannot go back to its row; "
                                       "it stays kept.");
                break;
            case EngineStays::No:
                break;
            }
            if (isOn(cueSwitch) && !kept->cueMoves.empty() && !cuesBlocked) {
                text += QStringLiteral(" The cue points of each library follow its own grid: those Analyse again "
                                       "moved went back in rekordbox and stay where they are in Engine.");
            }
            saveNotesOf(ctx).libraries[id()] = text;
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
