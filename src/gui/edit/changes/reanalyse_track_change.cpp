// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/reanalyse_track_change.hpp"

#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "application/path_key.hpp"
#include "application/ports/analysis_writer.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "domain/cue_tolerance.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;
using infrastructure::rekordbox::AnlzFile;

constexpr const char *Label = "track-reanalysis";

std::string exportPdbUnder(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

bool isFile(const std::string &path)
{
    std::error_code ec;
    return fs::is_regular_file(pathFromUtf8(path), ec);
}

std::string gridInWords(application::BeatGridWriteResult::Outcome outcome)
{
    using Outcome = application::BeatGridWriteResult::Outcome;
    switch (outcome) {
    case Outcome::Written:
        return "grid written";
    case Outcome::AlreadySame:
        return "grid already the same";
    case Outcome::RowLocked:
        return "grid locked on the player, left alone";
    case Outcome::RowEditedOnEngine:
        return "grid moved on the player, left alone";
    case Outcome::CannotKeepEngines:
        return "the player's analysis could not be kept, left alone";
    case Outcome::RowNotAnalysed:
        return "row not analysed and not completed";
    case Outcome::RowUnreadable:
        return "row unreadable, left alone";
    case Outcome::NoSourceGrid:
        return "no grid";
    case Outcome::NoSampleRate:
        return "no sample rate";
    case Outcome::UnsupportedSchema:
        return "Engine library too old";
    case Outcome::NotRequested:
        return "grid not asked for";
    }
    return "grid: unknown outcome";
}

}  // namespace

ReanalyseTrackChange::ReanalyseTrackChange(std::string audioFile, std::string stickPath, QString stickRoot,
                                           QString pioneerRoot, QString enginePath,
                                           std::vector<AnalysisFileOf> analysisFiles,
                                           std::shared_ptr<const ImportAnalysis> analysis, std::string analysedKey,
                                           QString title, QString artist, int itemCountHint)
    : m_audioFile(std::move(audioFile)), m_stickPath(std::move(stickPath)), m_stickRoot(std::move(stickRoot)),
      m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)),
      m_analysisFiles(std::move(analysisFiles)), m_analysis(std::move(analysis)), m_analysedKey(std::move(analysedKey)),
      m_title(std::move(title)), m_artist(std::move(artist)), m_itemCountHint(itemCountHint)
{
    if (m_audioFile.empty() || m_stickRoot.isEmpty() || m_stickPath.empty() || m_stickPath.front() != '/') {
        throw std::invalid_argument("ReanalyseTrackChange: a file on a stick is required");
    }
    if (!m_analysis || m_analysis->beats.empty()) {
        throw std::invalid_argument("ReanalyseTrackChange: an analysis with a grid is required");
    }
    for (const AnalysisFileOf &file : m_analysisFiles) {
        if (m_pioneerRoot.isEmpty() || file.datPath.rfind("/PIONEER/", 0) != 0
            || (file.library != QLatin1String("rekordbox") && file.library != QLatin1String("onelibrary"))) {
            throw std::invalid_argument("ReanalyseTrackChange: \"" + file.datPath + "\" is no analysis file of a "
                                        "rekordbox library on this stick");
        }
    }
}

QString ReanalyseTrackChange::clockText(double ms)
{
    const bool before = ms < 0.0;
    const auto whole = static_cast<long long>(std::llround(std::abs(ms)));
    return QStringLiteral("%1%2:%3.%4")
        .arg(before ? QStringLiteral("-") : QString())
        .arg(whole / 60000)
        .arg((whole / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(whole % 1000, 3, 10, QLatin1Char('0'));
}

QString ReanalyseTrackChange::cueLabel(domain::CuePoint::Kind kind, int hotCueNumber, bool isLoop)
{
    if (kind == domain::CuePoint::Kind::Hot) {
        return (isLoop ? QStringLiteral("Hot loop %1") : QStringLiteral("Hot cue %1")).arg(hotCueNumber);
    }
    return isLoop ? QStringLiteral("Memory loop") : QStringLiteral("Memory cue");
}

QString ReanalyseTrackChange::idFor(const std::string &stickPath)
{
    return QStringLiteral("addcue:reanalyse-track:") + QString::fromStdString(application::normalizedPathKey(stickPath));
}

QString ReanalyseTrackChange::unitKeyFor(const std::string &stickPath)
{
    return QStringLiteral("reanalyse-track:") + QString::fromStdString(application::normalizedPathKey(stickPath));
}

QString ReanalyseTrackChange::id() const
{
    return idFor(m_stickPath);
}

QString ReanalyseTrackChange::owner() const
{
    // Browse stages it beside its playlist edits and imports.
    return QStringLiteral("addcue");
}

QString ReanalyseTrackChange::description() const
{
    return QStringLiteral("Analyse \"%1\" again").arg(subject());
}

QString ReanalyseTrackChange::subject() const
{
    const QString title = m_title.isEmpty() ? pathToQString(pathFromUtf8(m_audioFile).filename()) : m_title;
    return m_artist.isEmpty() ? title : title + QStringLiteral(", ") + m_artist;
}

QString ReanalyseTrackChange::unit() const
{
    return QStringLiteral("tracks");
}

QString ReanalyseTrackChange::verb() const
{
    return QStringLiteral("analysed again");
}

QStringList ReanalyseTrackChange::unitKeys() const
{
    return {id(), unitKeyFor(m_stickPath)};
}

QStringList ReanalyseTrackChange::formatsTouched() const
{
    QStringList formats;
    for (const AnalysisFileOf &file : m_analysisFiles) {
        if (!formats.contains(file.library)) {
            formats << file.library;
        }
    }
    if (!m_enginePath.isEmpty()) {
        formats << QStringLiteral("engine");
    }
    return formats;
}

std::vector<std::string> ReanalyseTrackChange::anlzFilesOf(const std::string &datPath) const
{
    const fs::path root = pathFromUtf8(m_stickRoot.toStdString());
    const std::string stem = datPath.substr(1, datPath.size() - 4);
    std::vector<std::string> files;
    for (const char *extension : {"DAT", "EXT", "2EX"}) {
        files.push_back(pathToUtf8(root / pathFromUtf8(stem + extension)));
    }
    return files;
}

std::string ReanalyseTrackChange::ledgerStickRoot() const
{
    return infrastructure::paths::normalStickRootForCatalogPath(
        (m_pioneerRoot.isEmpty() ? m_enginePath : m_pioneerRoot).toStdString());
}

std::string ReanalyseTrackChange::ledgerKey() const
{
    const std::string relative = application::stickRelativePathOf(m_audioFile, ledgerStickRoot());
    return relative.empty() ? std::string() : application::normalizedPathKey(relative);
}

std::vector<BackupTarget> ReanalyseTrackChange::filesToBackup(SaveContext &) const
{
    std::vector<BackupTarget> targets;
    std::set<std::string> libraries;
    for (const AnalysisFileOf &file : m_analysisFiles) {
        libraries.insert(file.library.toStdString());
        // Each noted absent where it is not there: a .2EX the track lacks
        // is made, and Undo takes it away again. A missing .DAT or .EXT is
        // never made, so noting it absent restores nothing.
        for (const std::string &anlz : anlzFilesOf(file.datPath)) {
            targets.push_back({anlz, Label, true});
        }
    }
    const std::string pioneer = m_pioneerRoot.toStdString();
    if (libraries.count("rekordbox") > 0) {
        targets.push_back({exportPdbUnder(pioneer), Label});
    }
    if (libraries.count("onelibrary") > 0) {
        targets.push_back({infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label});
    }
    if (!m_enginePath.isEmpty()) {
        targets.push_back({FormatWriteSession::databaseFileFor("engine", m_enginePath.toStdString()), Label});
        // What the analysis writer may write beside the row: the player's
        // analysis kept, and the ledger's line for the grid.
        const std::string stickRoot = ledgerStickRoot();
        if (const std::string kept = keptEngineAnalysisFileOf(stickRoot, ledgerKey()); !kept.empty()) {
            targets.push_back({kept, Label, true});
        }
        targets.push_back({beatGridLedgerFileOf(stickRoot), Label, true});
    }
    return targets;
}

namespace
{

// What this save's Analyse again changes left out, for the summary: the
// tracks whose cue points stayed where they were though the person asked
// for them to move, and the tracks where a library was left as it was.
// One hook for the save, counting only the changes that landed.
struct ReanalysisNotes
{
    std::map<QString, std::string> cuesLeft;
    std::map<QString, std::string> librariesLeft;
};

ReanalysisNotes &saveNotesOf(SaveContext &ctx)
{
    bool created = false;
    auto &notes = ctx.sharedForWholeSave<ReanalysisNotes>("track-reanalysis:notes", [&]() {
        created = true;
        return std::make_unique<ReanalysisNotes>();
    });
    if (created) {
        ctx.onAfterCommit([&ctx](const SaveContext::AfterCommit &after) {
            const auto *notes = ctx.sharedForWholeSaveIfPresent<ReanalysisNotes>("track-reanalysis:notes");
            if (notes == nullptr) {
                return;
            }
            const auto landed = [&](const std::map<QString, std::string> &of) {
                int n = 0;
                for (const auto &[id, why] : of) {
                    n += after.appliedIds.contains(id) && !after.skippedIds.contains(id) ? 1 : 0;
                }
                return n;
            };
            if (const int n = landed(notes->cuesLeft); n > 0) {
                ctx.addSummaryNote(
                    n == 1 ? QStringLiteral("The cue points of one track analysed again were left where they are: they "
                                            "changed since the review, or the libraries hold different ones.")
                           : QStringLiteral("The cue points of %1 tracks analysed again were left where they are: they "
                                            "changed since the review, or the libraries hold different ones.")
                                 .arg(n));
            }
            if (const int n = landed(notes->librariesLeft); n > 0) {
                ctx.addSummaryNote(n == 1 ? QStringLiteral("On one track analysed again a library was left as it was "
                                                           "(the log says which, and why).")
                                          : QStringLiteral("On %1 tracks analysed again a library was left as it was "
                                                           "(the log says which, and why).")
                                                .arg(n));
            }
        });
    }
    return notes;
}

// The same cue points, in any order: kind, pad, loop and places within a
// millisecond (the rounding rekordbox's files store).
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

// export.pdb's track rows by their file, read once for a save: which rows
// a file has does not change while Analyse again writes tempos and keys.
struct PdbRowsByFile
{
    explicit PdbRowsByFile(const std::string &pdb)
    {
        for (const auto &row : infrastructure::rekordbox::PdbRowWriter(pdb).trackPathRows()) {
            rows[application::normalizedPathKey(row.filePath)].push_back(row.id);
        }
    }
    std::map<std::string, std::vector<std::uint32_t>> rows;
};

bool engineTookGrid(const application::AnalysisImportResult &result)
{
    using Outcome = application::BeatGridWriteResult::Outcome;
    return result.grid.outcome == Outcome::Written || result.grid.outcome == Outcome::AlreadySame;
}

}  // namespace

namespace
{

ReanalyseTrackChange::StepHook &stepHook()
{
    static ReanalyseTrackChange::StepHook hook;
    return hook;
}

void reached(const std::string &step)
{
    if (stepHook()) {
        stepHook()(step);
    }
}

}  // namespace

void ReanalyseTrackChange::setStepHookForTesting(StepHook hook)
{
    stepHook() = std::move(hook);
}

std::optional<std::vector<domain::CuePoint>> ReanalyseTrackChange::engineCuesOntoNewGrid(
    const application::CueGridPlan &review, const std::vector<domain::CuePoint> &cues, const std::vector<double> &oldBeats,
    const std::vector<double> &newBeats, double toleranceMs)
{
    const auto near = [&](double a, double b) { return std::abs(a - b) < toleranceMs; };
    // Engine's pads: hot cues and hot loops, planned on their own, so that
    // its one main cue cannot make the pads' plan refuse.
    std::vector<std::size_t> padAt;
    std::vector<domain::CuePoint> pads;
    for (std::size_t i = 0; i < cues.size(); ++i) {
        if (cues[i].kind == domain::CuePoint::Kind::Hot) {
            padAt.push_back(i);
            pads.push_back(cues[i]);
        }
    }
    const application::CueGridPlan plan = application::planCueGridMoves(pads, oldBeats, newBeats);
    if (plan.verdict == application::CueGridPlan::Verdict::GridChangedTooMuch) {
        return std::nullopt;
    }
    std::vector<bool> taken(plan.moves.size(), false);
    // Every pad the review moves, on the same pad in Engine (an Engine
    // row that lacks it holds other cue points than the review showed).
    for (const application::CueGridMove &shown : review.moves) {
        if (shown.kind != domain::CuePoint::Kind::Hot) {
            continue;
        }
        bool found = false;
        for (std::size_t i = 0; i < plan.moves.size() && !found; ++i) {
            const application::CueGridMove &move = plan.moves[i];
            if (!taken[i] && move.hotCueNumber == shown.hotCueNumber && move.isLoop == shown.isLoop
                && move.beat == shown.beat && near(move.oldMs, shown.oldMs) && near(move.newMs, shown.newMs)
                && (!move.isLoop || (move.endBeat == shown.endBeat && near(move.newEndMs, shown.newEndMs)))) {
                taken[i] = true;
                found = true;
            }
        }
        if (!found) {
            return std::nullopt;
        }
    }
    // Any other pad of Engine's that would move is a rekordbox memory cue
    // (or loop) Engine DJ's import put on a free pad, as the sync reads
    // it (domain/engine_cue_translation.hpp): it has to be one the review
    // moves, at the same place.
    for (std::size_t i = 0; i < plan.moves.size(); ++i) {
        if (taken[i]) {
            continue;
        }
        const application::CueGridMove &move = plan.moves[i];
        bool found = false;
        for (const application::CueGridMove &shown : review.moves) {
            found = found
                    || (shown.kind == domain::CuePoint::Kind::Memory && shown.isLoop == move.isLoop
                        && near(move.oldMs, shown.oldMs) && near(move.newMs, shown.newMs));
        }
        if (!found) {
            return std::nullopt;
        }
    }
    std::vector<domain::CuePoint> moved = cues;
    const std::vector<domain::CuePoint> movedPads = application::cuesMovedOntoNewGrid(pads, plan);
    for (std::size_t i = 0; i < padAt.size(); ++i) {
        moved[padAt[i]] = movedPads[i];
    }
    // Engine's one main cue follows a memory cue the review moves from
    // its place; at any other place it stays, and is no reason to refuse:
    // rekordbox's several memory cues have one main cue in Engine.
    for (domain::CuePoint &cue : moved) {
        if (cue.kind != domain::CuePoint::Kind::Memory || cue.isLoop) {
            continue;
        }
        for (const application::CueGridMove &shown : review.moves) {
            if (shown.kind == domain::CuePoint::Kind::Memory && !shown.isLoop && near(cue.positionMs, shown.oldMs)) {
                cue.positionMs += shown.newMs - shown.oldMs;
                break;
            }
        }
    }
    return moved;
}

bool ReanalyseTrackChange::cuesMatchTheReview(const application::CueGridPlan &review,
                                              const application::CueGridPlan &plan, double toleranceMs)
{
    if (plan.verdict != application::CueGridPlan::Verdict::Moves || plan.moves.size() != review.moves.size()) {
        return false;
    }
    std::vector<bool> taken(plan.moves.size(), false);
    const auto near = [&](double a, double b) { return std::abs(a - b) < toleranceMs; };
    for (const application::CueGridMove &shown : review.moves) {
        bool found = false;
        for (std::size_t i = 0; i < plan.moves.size() && !found; ++i) {
            const application::CueGridMove &move = plan.moves[i];
            const bool same = !taken[i] && move.kind == shown.kind && move.isLoop == shown.isLoop
                              && (move.kind != domain::CuePoint::Kind::Hot || move.hotCueNumber == shown.hotCueNumber)
                              && move.beat == shown.beat && near(move.oldMs, shown.oldMs) && near(move.newMs, shown.newMs)
                              && (!move.isLoop
                                  || (move.endBeat == shown.endBeat && near(move.oldEndMs, shown.oldEndMs)
                                      && near(move.newEndMs, shown.newEndMs)));
            if (same) {
                taken[i] = true;
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

std::vector<RekordboxWrite> ReanalyseTrackChange::rekordboxWrites() const
{
    if (!m_written) {
        return {};
    }
    return {*m_written};
}

ChangeOutcome ReanalyseTrackChange::apply(SaveContext &ctx)
{
    m_written.reset();
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string name = "\"" + subject().toStdString() + "\" (" + m_stickPath + ")";
    const std::string fileKey = application::normalizedPathKey(m_audioFile);
    // What was written, what was the same already, and the libraries left
    // as they were with the reason: for the log line and the outcome.
    std::vector<std::string> done;
    std::vector<std::string> same;
    std::vector<std::string> left;
    const auto leftAlone = [&](const std::string &why) {
        ctx.log().record(std::string(Label) + ": " + name + ": " + why);
    };

    // Everything that can refuse is asked before the first write: the
    // files built and merged in memory, each against the file as it is
    // now, Engine's rows asked in a preview, and every library's cues
    // planned. A change that fails takes the save back, so nothing of it
    // may be half on the stick.
    const RekordboxAnalysisFiles built = rekordboxAnalysisFilesFor(*m_analysis, m_audioFile, m_stickPath, 0.0);
    if (!built.files || built.beats.empty()) {
        leftAlone("left as it is: the analysis has no grid on rekordbox's timeline"
                  + (built.error.empty() ? std::string() : " (" + built.error + ")"));
        return ChangeOutcome::skip();
    }

    struct MergedFiles
    {
        std::string datPath;
        std::vector<std::string> paths;  // .DAT, .EXT, .2EX
        infrastructure::rekordbox::MergedTrackAnlzFiles merged;
        bool twoExIsNew = false;
    };
    std::vector<MergedFiles> merges;
    // The rekordbox libraries whose every analysis file takes the new
    // analysis: only their rows get its tempo.
    std::map<QString, bool> libraryTakesIt;
    for (const AnalysisFileOf &file : m_analysisFiles) {
        auto taken = libraryTakesIt.emplace(file.library, true).first;
        const auto known = std::find_if(merges.begin(), merges.end(), [&](const MergedFiles &m) {
            return application::normalizedPathKey(m.datPath) == application::normalizedPathKey(file.datPath);
        });
        if (known != merges.end()) {
            continue;
        }
        const std::vector<std::string> paths = anlzFilesOf(file.datPath);
        infrastructure::rekordbox::TrackAnlzFiles old;
        try {
            if (isFile(paths[0])) {
                old.dat = AnlzFile::readRaw(paths[0]);
            }
            if (isFile(paths[1])) {
                old.ext = AnlzFile::readRaw(paths[1]);
            }
            if (isFile(paths[2])) {
                old.twoEx = AnlzFile::readRaw(paths[2]);
            }
        } catch (const std::exception &e) {
            left.push_back(file.library.toStdString() + ": the analysis files at " + file.datPath
                           + " could not be read (" + e.what() + ")");
            taken->second = false;
            continue;
        }
        const auto merged =
            infrastructure::rekordbox::mergeTrackAnlzFiles(old, built.files->dat, built.files->ext, built.files->twoEx);
        if (!merged) {
            left.push_back(file.library.toStdString() + ": " + file.datPath + " is not on the stick");
            taken->second = false;
            continue;
        }
        merges.push_back({file.datPath, paths, *merged, !old.twoEx});
    }
    // A library that names a file that could not take it keeps its rows as
    // they are; its other files are not written either.
    std::vector<MergedFiles> toWrite;
    for (const MergedFiles &m : merges) {
        bool wanted = false;
        for (const AnalysisFileOf &file : m_analysisFiles) {
            wanted = wanted
                     || (libraryTakesIt[file.library]
                         && application::normalizedPathKey(file.datPath) == application::normalizedPathKey(m.datPath));
        }
        if (wanted) {
            toWrite.push_back(m);
        }
    }
    const double bpm = m_analysis->bpm;
    const bool toDeviceLibrary = libraryTakesIt.count(QStringLiteral("rekordbox")) > 0
                                 && libraryTakesIt[QStringLiteral("rekordbox")];
    const bool toOneLibrary = libraryTakesIt.count(QStringLiteral("onelibrary")) > 0
                              && libraryTakesIt[QStringLiteral("onelibrary")];
    const bool hasEngine = !m_enginePath.isEmpty()
                           && isFile(FormatWriteSession::databaseFileFor("engine", m_enginePath.toStdString()));
    auto &catalogs = LibraryCatalogCache::instance();

    try {
        // Engine's rows, read from the save's own copy of m.db (a scratch
        // copy in a large save), and what the writer would do to each.
        struct EngineRow
        {
            std::int64_t id = 0;
            domain::Track track;
            application::RekordboxAnalysis analysis;
            application::AnalysisImportOptions options;
            application::AnalysisImportResult preview;
        };
        std::vector<EngineRow> engineRows;
        FormatWriteSession *engineSession = nullptr;
        const std::string stickRoot = ledgerStickRoot();
        const std::string key = ledgerKey();
        if (hasEngine) {
            engineSession = &sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, Label);
            infrastructure::engine::LibdjinteropEngineReader reader(engineSession->writeRoot());
            reader.setAudioRoot(engineSession->realRoot());
            // A row with no stored rate (one the player has not analysed)
            // is read at the file's own rate, which the analysis decoded,
            // not at 44.1 kHz: a 48 kHz file's cues would read 8.8% late.
            const double fileRate = static_cast<double>(m_analysis->waveform.sampleRateHz);
            reader.setSampleRateSource([fileRate](const std::string &) {
                return fileRate > 0.0 ? std::optional<double>(fileRate) : std::nullopt;
            });
            const auto &ledger = saveBeatGridLedger(ctx, stickRoot);
            for (const std::int64_t id :
                 infrastructure::engine::engineTrackIdsForFile(engineSession->writeRoot(), engineSession->realRoot(),
                                                               m_audioFile)) {
                EngineRow row;
                row.id = id;
                row.track = reader.readTrack(std::to_string(id)).value_or(domain::Track{});
                row.analysis.beats = built.beats;
                row.analysis.threeBandPreview = built.files->threeBandPreview;
                row.analysis.colourPreview = built.files->colourPreview;
                row.analysis.scrollEntries = built.files->scrollEntries;
                if (bpm > 0.0) {
                    row.analysis.bpm = bpm;
                }
                // Only into a row with no key: the writer puts it in when it
                // completes the row, and never on one the player analysed.
                if (!m_analysedKey.empty() && row.track.key.empty()) {
                    if (const auto parsed = infrastructure::engine::parseRekordboxKey(m_analysedKey)) {
                        row.analysis.key = static_cast<int>(*parsed);
                    }
                }
                row.options.fileSampleRateHz = static_cast<double>(m_analysis->waveform.sampleRateHz);
                row.options.fileIsMp3 = built.isMp3;
                row.options.fileSampleCount = built.sampleCount;
                row.options.fileCountRequired = !built.isMp3;
                row.options.tempoWithGrid = true;
                row.options.keepKey = key;
                if (const auto own = ledger.find(key); !key.empty() && own != ledger.end()) {
                    row.options.ownGridFingerprint = own->second;
                }
                application::AnalysisImportOptions preview = row.options;
                preview.previewOnly = true;
                row.preview = engineAnalysisWriterFor(ctx, *engineSession, stickRoot)
                                  .importAnalysis(std::to_string(id), row.analysis, preview);
                engineRows.push_back(std::move(row));
            }
            if (engineRows.empty()) {
                left.push_back("Engine: it has no row for the file");
            }
        }

        // The rows of rekordbox's two libraries for the file, as they are
        // now. Every analysis file they name was declared for backup when
        // the change was staged; a row named since (a library rewritten in
        // between) could have its files written with no backup behind
        // them, so the change leaves the track alone.
        //
        // Read for this file alone, never the whole catalog (a playlist of
        // hundreds would read the stick's catalogs and every track's cues
        // once per track): DeviceLibrary's rows by file from one pass over
        // export.pdb for the whole save, each row's analysis file from the
        // save's index of them, OneLibrary's rows from the save's writer,
        // and the cues from the analysis files as they are on the stick
        // now, which is what a cue change earlier in this save left.
        struct RowCues
        {
            std::string sourceId;
            std::vector<domain::CuePoint> cues;
            bool unreadable = false;
        };
        std::vector<RowCues> deviceLibraryRows;
        std::vector<RowCues> oneLibraryRows;
        std::set<std::string> declaredFiles;
        for (const AnalysisFileOf &of : m_analysisFiles) {
            for (const std::string &file : anlzFilesOf(of.datPath)) {
                declaredFiles.insert(application::normalizedPathKey(file));
            }
        }
        const auto undeclared = [&](const std::string &file) {
            leftAlone("left as it is, nothing written: a row now names the analysis file " + file
                      + ", which was not one of the track's when it was analysed");
            return ChangeOutcome::skip();
        };
        if (!pioneer.empty() && isFile(exportPdbUnder(pioneer))) {
            const auto &byFile = ctx.sharedForWholeSave<PdbRowsByFile>(
                "track-reanalysis:pdb-rows:" + application::normalizedPathKey(pioneer),
                [&]() { return std::make_unique<PdbRowsByFile>(exportPdbUnder(pioneer)); });
            const auto ids = byFile.rows.find(application::normalizedPathKey(m_stickPath));
            if (ids != byFile.rows.end()) {
                const auto *index = sharedAnlzPathIndex(ctx, m_pioneerRoot);
                const auto source = infrastructure::rekordbox::anlzSourceForPioneerRoot(pioneer);
                for (const std::uint32_t id : ids->second) {
                    const std::optional<std::string> analyzePath =
                        index ? index->pathFor(id) : infrastructure::rekordbox::findAnlzPathForTrackId(pioneer, id);
                    RowCues row{std::to_string(id), {}, false};
                    if (analyzePath && !analyzePath->empty()) {
                        for (const std::string &file : anlzFilesOf(*analyzePath)) {
                            if (isFile(file) && declaredFiles.count(application::normalizedPathKey(file)) == 0) {
                                return undeclared(*analyzePath);
                            }
                        }
                        try {
                            row.cues = infrastructure::rekordbox::readAnalysisFileCues(*source, *analyzePath)
                                           .value_or(std::vector<domain::CuePoint>{});
                        } catch (const infrastructure::rekordbox::AnalysisFileUnreadable &) {
                            row.unreadable = true;
                        }
                    }
                    deviceLibraryRows.push_back(std::move(row));
                }
            }
        }
        if (!pioneer.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            std::vector<std::int64_t> ids;
            try {
                ids = writer.contentIdsForFile(m_audioFile);
            } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                // No row: nothing of OneLibrary's to compare.
            }
            if (!ids.empty()) {
                for (const std::string &file : writer.cueFilesForPath(m_audioFile)) {
                    if (declaredFiles.count(application::normalizedPathKey(file)) == 0) {
                        return undeclared(file);
                    }
                }
            }
            for (const std::int64_t id : ids) {
                RowCues row{std::to_string(id), {}, false};
                try {
                    row.cues = writer.analysisFileCuesOf(id).value_or(std::vector<domain::CuePoint>{});
                } catch (const std::exception &) {
                    row.unreadable = true;
                }
                oneLibraryRows.push_back(std::move(row));
            }
        }

        // The cue part, all or nothing across the libraries (see the class
        // comment): every library that holds cues for the file planned
        // against the review's grids, before anything is written. Each is
        // compared for the kinds it can hold.
        struct RekordboxCues
        {
            std::string sourceId;
            std::vector<domain::CuePoint> moved;
        };
        std::vector<RekordboxCues> deviceLibraryCues;
        std::optional<std::vector<domain::CuePoint>> oneLibraryCues;
        std::vector<std::pair<std::int64_t, std::vector<domain::CuePoint>>> engineCues;
        std::vector<application::CueGridMove> movedForLog;
        bool cuesMove = false;
        // The switch and whose it is, read once for the whole change.
        const int cueSwitch = m_cueSwitch.load();
        if (isOn(cueSwitch) && m_cueReview.plan.verdict == application::CueGridPlan::Verdict::Moves) {
            const double tolerance = domain::cueToleranceMsFor(m_cueReview.oldBpm, m_cueReview.oldBpm);
            std::string whyNot;
            const auto refuse = [&](const std::string &why) {
                if (whyNot.empty()) {
                    whyNot = why;
                }
            };
            const auto rekordboxSide = [&](const std::string &library, const RowCues &track)
                -> std::optional<std::vector<domain::CuePoint>> {
                if (track.unreadable) {
                    refuse(library + "'s cue points could not be read");
                    return std::nullopt;
                }
                if (track.cues.empty()) {
                    return std::nullopt;
                }
                const application::CueGridPlan plan =
                    application::planCueGridMoves(track.cues, m_cueReview.oldBeats, m_cueReview.newBeats);
                if (!cuesMatchTheReview(m_cueReview.plan, plan, tolerance)) {
                    refuse(library + " holds other cue points than the review showed");
                    return std::nullopt;
                }
                if (movedForLog.empty()) {
                    movedForLog = plan.moves;
                }
                return application::cuesMovedOntoNewGrid(track.cues, plan);
            };
            // A library that holds cues and keeps its old grid: its cues
            // belong to that grid, and the others' cannot leave them.
            const auto keepsOldGrid = [&](const char *library, const std::vector<RowCues> &rows) {
                for (const RowCues &row : rows) {
                    if (!row.cues.empty()) {
                        refuse(std::string(library) + " keeps its old grid, and its cue points with it");
                    }
                }
            };
            // One file's rows share its cues on a player: rows that hold
            // different sets cannot be given one moved set, nor one
            // library's set written over the other's, which would take away
            // the cues only the other holds. The ordinary case, both
            // libraries naming the same analysis file, agrees by itself.
            const auto oneSet = [&](const std::vector<RowCues> &rows, const char *which) {
                for (const RowCues &row : rows) {
                    if (!sameCueSet(row.cues, rows.front().cues)) {
                        refuse(std::string("its ") + which + " rows hold different cue points");
                    }
                }
            };
            oneSet(deviceLibraryRows, "DeviceLibrary");
            oneSet(oneLibraryRows, "OneLibrary");
            if (!deviceLibraryRows.empty() && !oneLibraryRows.empty()
                && !sameCueSet(deviceLibraryRows.front().cues, oneLibraryRows.front().cues)) {
                refuse("DeviceLibrary and OneLibrary hold different cue points");
            }
            if (toDeviceLibrary) {
                for (const RowCues &track : deviceLibraryRows) {
                    if (auto moved = rekordboxSide("DeviceLibrary", track)) {
                        deviceLibraryCues.push_back({track.sourceId, std::move(*moved)});
                    }
                }
            } else {
                keepsOldGrid("DeviceLibrary", deviceLibraryRows);
            }
            if (toOneLibrary) {
                for (const RowCues &track : oneLibraryRows) {
                    if (auto moved = rekordboxSide("OneLibrary", track)) {
                        oneLibraryCues = std::move(*moved);
                    }
                }
            } else {
                keepsOldGrid("OneLibrary", oneLibraryRows);
            }
            for (const EngineRow &row : engineRows) {
                if (row.track.cues.empty()) {
                    continue;
                }
                if (!engineTookGrid(row.preview)) {
                    refuse("Engine keeps its own grid (" + gridInWords(row.preview.grid.outcome) + ")");
                    continue;
                }
                auto moved = engineCuesOntoNewGrid(m_cueReview.plan, row.track.cues, m_cueReview.oldBeats,
                                                   m_cueReview.newBeats, tolerance);
                if (!moved) {
                    refuse("Engine holds other cue points than the review showed");
                } else if (!sameCueSet(*moved, row.track.cues)) {
                    engineCues.emplace_back(row.id, std::move(*moved));
                }
            }
            if (movedForLog.empty() && !engineCues.empty()) {
                movedForLog = m_cueReview.plan.moves;
            }
            cuesMove = whyNot.empty() && !movedForLog.empty();
            if (!cuesMove) {
                const std::string why = whyNot.empty() ? "no library holds the cue points the review showed" : whyNot;
                leftAlone("cue points left where they are, though moving them was asked for: " + why);
                saveNotesOf(ctx).cuesLeft[id()] = why;
            }
        }

        // The writes, each library told to the catalog cache as it lands.
        for (const MergedFiles &m : toWrite) {
            const auto &files = m.merged.files;
            files.dat->writeRaw(m.paths[0]);
            if (files.ext) {
                files.ext->writeRaw(m.paths[1]);
            }
            if (m.twoExIsNew) {
                if (!infrastructure::writeFileDurablyAtomic(m.paths[2], files.twoEx->toBytes())) {
                    return ChangeOutcome::failure(
                        QStringLiteral("Could not write the analysis file %1.").arg(QString::fromStdString(m.paths[2])));
                }
            } else {
                files.twoEx->writeRaw(m.paths[2]);
            }
            done.push_back("analysis files " + m.datPath + (m.twoExIsNew ? " (.2EX made)" : "")
                           + (m.merged.phrasesKept ? ", phrases kept"
                              : m.merged.phrasesRemoved ? ", phrases taken out (they counted the old grid's beats)"
                                                        : ""));
        }
        if (!toWrite.empty()) {
            catalogs.invalidateWithOneLibraryMirror("rekordbox", pioneer);
        }

        if (toDeviceLibrary) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, m_itemCountHint, Label);
            infrastructure::rekordbox::PdbRowWriter rows(exportPdbUnder(session.writeRoot()));
            const std::vector<std::uint32_t> ids = rows.trackIdsWithFilePath(m_stickPath);
            int keys = 0;
            for (const std::uint32_t row : ids) {
                bool keyWritten = false;
                rows.setTrackTempoAndKey(row, bpm, m_analysedKey, &keyWritten);
                keys += keyWritten ? 1 : 0;
            }
            if (!ids.empty()) {
                if (!rows.commit()) {
                    return ChangeOutcome::failure(QStringLiteral("Could not write export.pdb for %1.").arg(subject()));
                }
                session.noteItemApplied();
                catalogs.invalidate("rekordbox", pioneer);
                done.push_back("export.pdb: tempo of " + std::to_string(ids.size()) + " row(s)"
                               + (keys > 0 ? ", key " + m_analysedKey + " into " + std::to_string(keys)
                                                 + " that named none"
                                           : ", keys left as they are"));
            } else {
                left.push_back("DeviceLibrary: export.pdb has no row for it any more");
            }
        }
        if (toOneLibrary) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
            try {
                int keys = 0;
                const int rows =
                    sharedOneLibraryWriter(ctx, pioneer).setTempoAndKeyForPath(m_audioFile, bpm, m_analysedKey, &keys);
                catalogs.invalidateWithOneLibraryMirror("onelibrary", pioneer);
                done.push_back("exportLibrary.db: tempo of " + std::to_string(rows) + " row(s)"
                               + (keys > 0 ? ", key " + m_analysedKey + " into " + std::to_string(keys)
                                                 + " that named none"
                                           : ", keys left as they are"));
            } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                left.push_back("OneLibrary: exportLibrary.db has no row for it any more");
            }
        }

        std::set<std::int64_t> engineRowsOnNewGrid;
        for (const EngineRow &row : engineRows) {
            // A row whose grid stays (locked or moved on the player, its
            // own analysis not to be kept, unreadable) is left whole: a
            // new overview under the DJ's grid would be half an analysis.
            if (!engineTookGrid(row.preview) && !row.preview.completedRow) {
                left.push_back("Engine id " + std::to_string(row.id) + ": " + gridInWords(row.preview.grid.outcome));
                continue;
            }
            reached("engine-grid:" + std::to_string(row.id));
            const application::AnalysisImportResult written =
                engineAnalysisWriterFor(ctx, *engineSession, stickRoot)
                    .importAnalysis(std::to_string(row.id), row.analysis, row.options);
            using Outcome = application::BeatGridWriteResult::Outcome;
            const bool gridWritten = written.grid.outcome == Outcome::Written;
            const bool waveformWritten = written.waveform == application::AnalysisImportResult::Waveform::Written;
            if (engineTookGrid(written)) {
                recordGridInLedgerAfterCommit(ctx, stickRoot, id(), key, written.grid.gridFingerprint);
                engineRowsOnNewGrid.insert(row.id);
            }
            const std::string what = "Engine id " + std::to_string(row.id) + ": " + gridInWords(written.grid.outcome)
                                     + (written.completedRow ? ", row completed" : "")
                                     + (written.tempoWritten ? ", tempo written" : "")
                                     + (waveformWritten ? ", waveform written" : "")
                                     + (written.keptEngineAnalysis ? ", the player's analysis kept first" : "");
            if (gridWritten || waveformWritten || written.tempoWritten || written.completedRow) {
                engineSession->noteItemApplied();
                done.push_back(what);
            } else if (written.grid.outcome == Outcome::AlreadySame) {
                same.push_back(what);
            } else {
                left.push_back(what);
            }
        }
        if (!engineRows.empty()) {
            catalogs.invalidate("engine", m_enginePath.toStdString());
        }

        // Every grid of every library is written by now, and the cues come
        // last. An Engine row whose cues the plan moves has to have taken
        // the new grid for real, not only in the preview: one that did not
        // would keep cues on a grid that stayed while the others moved, so
        // the change fails, and the save goes back whole.
        if (cuesMove) {
            for (const auto &[row, moved] : engineCues) {
                (void)moved;
                if (engineRowsOnNewGrid.count(row) == 0) {
                    return ChangeOutcome::failure(
                        QStringLiteral("Engine's row %1 for %2 did not take the new grid its moved cue points belong to.")
                            .arg(row)
                            .arg(subject()));
                }
            }
        }

        if (cuesMove) {
            // A move the threshold turned on, not the person, is said to
            // be one.
            if (const int under = std::max(cueSwitch, 0); under > 0) {
                done.push_back("its cue points moved on their own, every move under " + std::to_string(under) + " ms");
            }
            const auto said = [&](const std::string &library) {
                for (const application::CueGridMove &move : movedForLog) {
                    std::string line = library + ": moved "
                                       + cueLabel(move.kind, move.hotCueNumber, move.isLoop).toStdString() + " from "
                                       + clockText(move.oldMs).toStdString() + " to "
                                       + clockText(move.newMs).toStdString();
                    if (move.isLoop) {
                        line += " (its end from " + clockText(move.oldEndMs).toStdString() + " to "
                                + clockText(move.newEndMs).toStdString() + ")";
                    }
                    done.push_back(line);
                }
            };
            if (!deviceLibraryCues.empty()) {
                // Every row's own files: two rows naming two analysis files
                // each take the move. The files are declared in
                // filesToBackup(); the writer reads them as written above.
                infrastructure::rekordbox::RekordboxCueWriter writer(pioneer, sharedAnlzPathIndex(ctx, m_pioneerRoot));
                for (const RekordboxCues &row : deviceLibraryCues) {
                    writer.writeHotCues(row.sourceId, row.moved);
                    reached("rekordbox-cues:" + row.sourceId);
                }
                if (toOneLibrary) {
                    const QString failed =
                        mirrorCuesOrExplain(sharedOneLibraryWriter(ctx, pioneer), m_audioFile, deviceLibraryCues.front().moved,
                                            ctx, Label, QStringLiteral("move the cue points"));
                    if (!failed.isEmpty()) {
                        return ChangeOutcome::failure(failed);
                    }
                }
                catalogs.invalidateWithOneLibraryMirror("rekordbox", pioneer);
                catalogs.invalidateWithOneLibraryMirror("onelibrary", pioneer);
                m_written = RekordboxWrite{m_audioFile, deviceLibraryCues.front().moved, std::nullopt};
                said(toOneLibrary ? "DeviceLibrary and OneLibrary" : "DeviceLibrary");
            } else if (oneLibraryCues) {
                sharedOneLibraryWriter(ctx, pioneer).writeCuesForPath(m_audioFile, *oneLibraryCues);
                catalogs.invalidateWithOneLibraryMirror("onelibrary", pioneer);
                m_written = RekordboxWrite{m_audioFile, *oneLibraryCues, std::nullopt};
                said("OneLibrary");
            }
            if (!engineCues.empty()) {
                for (const auto &[row, moved] : engineCues) {
                    engineAnalysisWriterFor(ctx, *engineSession, stickRoot).writeHotCues(std::to_string(row), moved);
                    reached("engine-cues:" + std::to_string(row));
                    // What Engine itself moved: its pads, and its main cue
                    // only where it followed a memory cue.
                    const auto before = std::find_if(engineRows.begin(), engineRows.end(),
                                                      [&](const EngineRow &r) { return r.id == row; });
                    for (std::size_t i = 0; before != engineRows.end() && i < moved.size()
                                            && i < before->track.cues.size();
                         ++i) {
                        const domain::CuePoint &was = before->track.cues[i];
                        if (std::abs(moved[i].positionMs - was.positionMs) >= application::CueMoveMinimumMs
                            || (was.isLoop && std::abs(moved[i].loopEndMs - was.loopEndMs) >= application::CueMoveMinimumMs)) {
                            done.push_back("Engine id " + std::to_string(row) + ": moved "
                                           + (was.kind == domain::CuePoint::Kind::Memory
                                                  ? std::string(was.isLoop ? "main loop" : "main cue")
                                                  : cueLabel(was.kind, was.hotCueNumber, was.isLoop).toStdString())
                                           + " from " + clockText(was.positionMs).toStdString() + " to "
                                           + clockText(moved[i].positionMs).toStdString());
                        }
                    }
                }
                engineSession->noteItemApplied();
                catalogs.invalidate("engine", m_enginePath.toStdString());
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }

    const auto joined = [](const std::vector<std::string> &parts) {
        std::string line;
        for (const std::string &part : parts) {
            line += (line.empty() ? "" : "; ") + part;
        }
        return line;
    };
    if (done.empty()) {
        leftAlone("left as it is, nothing written"
                  + (left.empty() && same.empty() ? std::string(": no library on the stick had an analysis of it to replace")
                                                  : ": " + joined(left) + (left.empty() || same.empty() ? "" : "; ")
                                                        + joined(same)));
        return ChangeOutcome::skip();
    }
    if (!left.empty()) {
        saveNotesOf(ctx).librariesLeft[id()] = joined(left);
    }
    ctx.log().record(std::string(Label) + ": analysed " + name + " again, " + QString::number(bpm, 'f', 2).toStdString()
                     + " BPM, analysed key " + (m_analysedKey.empty() ? std::string("none") : m_analysedKey) + ": "
                     + joined(done) + (same.empty() ? "" : "; " + joined(same))
                     + (left.empty() ? "" : "; left as it was: " + joined(left)));
    return ChangeOutcome::success();
}

// The strip of the review for one cue or loop: the new analysis's
// waveform a few beats either side of it, both grids, and the cue's old
// and new place, all on rekordbox's timeline and given from the strip's
// first column (in ms), so the view only draws.
QVariantMap ReanalyseTrackChange::reviewStrip(const application::CueGridMove &move) const
{
    const auto &review = m_cueReview;
    const domain::AudioWaveform &waveform = m_analysis->waveform;
    const auto &old = review.oldBeats;
    const double spacing = old.size() > move.beat + 1 ? old[move.beat + 1] - old[move.beat]
                           : old.size() >= 2          ? old[1] - old[0]
                                                      : 500.0;
    double from = std::min(move.oldMs, move.newMs);
    double to = std::max(move.oldMs, move.newMs);
    if (move.isLoop) {
        from = std::min({from, move.oldEndMs, move.newEndMs});
        to = std::max({to, move.oldEndMs, move.newEndMs});
    }
    from -= 2.0 * spacing;
    to += 2.0 * spacing;
    // Whole columns: entry i is at i / 150 s on the decoder's timeline,
    // the offset later on rekordbox's.
    const double columnMs = 1000.0 / domain::WaveformEntriesPerSecond;
    const auto entries = static_cast<long long>(waveform.entries.size());
    const long long first = std::clamp<long long>(
        static_cast<long long>(std::ceil((from - review.waveformOffsetMs) / columnMs)), 0, entries);
    const long long last = std::clamp<long long>(
        static_cast<long long>(std::floor((to - review.waveformOffsetMs) / columnMs)), first, entries);
    const double startMs = static_cast<double>(first) * columnMs + review.waveformOffsetMs;
    const double spanMs = static_cast<double>(last - first) * columnMs;
    QVariantList columns;
    for (long long i = first; i < last; ++i) {
        const domain::WaveformEntry &entry = waveform.entries[static_cast<std::size_t>(i)];
        columns << QVariantMap{{QStringLiteral("low"), std::min(1.0f, entry.low)},
                               {QStringLiteral("mid"), std::min(1.0f, entry.mid)},
                               {QStringLiteral("high"), std::min(1.0f, entry.high)}};
    }
    const auto inStrip = [&](const std::vector<double> &beats) {
        QVariantList shown;
        for (const double beat : beats) {
            if (beat >= startMs && beat <= startMs + spanMs) {
                shown << beat - startMs;
            }
        }
        return shown;
    };
    QVariantMap strip{{QStringLiteral("spanMs"), spanMs},
                      {QStringLiteral("columns"), columns},
                      {QStringLiteral("oldBeats"), inStrip(review.oldBeats)},
                      {QStringLiteral("newBeats"), inStrip(review.newBeats)},
                      {QStringLiteral("oldCueMs"), move.oldMs - startMs},
                      {QStringLiteral("newCueMs"), move.newMs - startMs}};
    if (move.isLoop) {
        strip.insert(QStringLiteral("oldEndMs"), move.oldEndMs - startMs);
        strip.insert(QStringLiteral("newEndMs"), move.newEndMs - startMs);
    }
    return strip;
}

// How far the grid moved, as one number: the middle of the beats' moves.
double ReanalyseTrackChange::gridShiftMs() const
{
    const auto &oldBeats = m_cueReview.oldBeats;
    const auto &newBeats = m_cueReview.newBeats;
    std::vector<double> shifts;
    for (std::size_t i = 0; i < std::min(oldBeats.size(), newBeats.size()); ++i) {
        shifts.push_back(newBeats[i] - oldBeats[i]);
    }
    if (shifts.empty()) {
        return 0.0;
    }
    std::nth_element(shifts.begin(), shifts.begin() + static_cast<std::ptrdiff_t>(shifts.size() / 2), shifts.end());
    return shifts[shifts.size() / 2];
}

}  // namespace seabass::gui
