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

bool engineTookGrid(const application::AnalysisImportResult &result)
{
    using Outcome = application::BeatGridWriteResult::Outcome;
    return result.grid.outcome == Outcome::Written || result.grid.outcome == Outcome::AlreadySame;
}

}  // namespace

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

        // The cue part, all or nothing across the libraries (see the class
        // comment): every library that holds cues for the file planned
        // against the review's grids, before anything is written.
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
        if (moveCues() && m_cueReview.plan.verdict == application::CueGridPlan::Verdict::Moves) {
            const double tolerance = domain::cueToleranceMsFor(m_cueReview.oldBpm, m_cueReview.oldBpm);
            std::string whyNot;
            const auto check = [&](const std::string &library, const domain::Track &track) {
                if (track.cues.empty() || !whyNot.empty()) {
                    return std::optional<std::vector<domain::CuePoint>>{};
                }
                const application::CueGridPlan plan =
                    application::planCueGridMoves(track.cues, m_cueReview.oldBeats, m_cueReview.newBeats);
                if (!cuesMatchTheReview(m_cueReview.plan, plan, tolerance)) {
                    whyNot = library + " holds other cue points than the review showed";
                    return std::optional<std::vector<domain::CuePoint>>{};
                }
                if (movedForLog.empty()) {
                    movedForLog = plan.moves;
                }
                return std::optional<std::vector<domain::CuePoint>>{application::cuesMovedOntoNewGrid(track.cues, plan)};
            };
            if (toDeviceLibrary) {
                for (const domain::Track &track : catalogs.tracksFor("rekordbox", pioneer, ctx.progress(), ctx.cancel())) {
                    if (application::normalizedPathKey(track.filePath) == fileKey) {
                        if (auto moved = check("DeviceLibrary", track)) {
                            deviceLibraryCues.push_back({track.sourceId, std::move(*moved)});
                        }
                    }
                }
            }
            if (toOneLibrary) {
                for (const domain::Track &track : catalogs.tracksFor("onelibrary", pioneer, ctx.progress(), ctx.cancel())) {
                    if (application::normalizedPathKey(track.filePath) == fileKey) {
                        if (auto moved = check("OneLibrary", track)) {
                            oneLibraryCues = std::move(*moved);
                        }
                    }
                }
            }
            for (const EngineRow &row : engineRows) {
                if (row.track.cues.empty()) {
                    continue;
                }
                if (!engineTookGrid(row.preview)) {
                    whyNot = whyNot.empty() ? "Engine keeps its own grid (" + gridInWords(row.preview.grid.outcome) + ")"
                                            : whyNot;
                    continue;
                }
                if (auto moved = check("Engine", row.track)) {
                    engineCues.emplace_back(row.id, std::move(*moved));
                }
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

        std::vector<std::int64_t> engineRowsOnNewGrid;
        for (const EngineRow &row : engineRows) {
            // A row whose grid stays (locked or moved on the player, its
            // own analysis not to be kept, unreadable) is left whole: a
            // new overview under the DJ's grid would be half an analysis.
            if (!engineTookGrid(row.preview) && !row.preview.completedRow) {
                left.push_back("Engine id " + std::to_string(row.id) + ": " + gridInWords(row.preview.grid.outcome));
                continue;
            }
            const application::AnalysisImportResult written =
                engineAnalysisWriterFor(ctx, *engineSession, stickRoot)
                    .importAnalysis(std::to_string(row.id), row.analysis, row.options);
            using Outcome = application::BeatGridWriteResult::Outcome;
            const bool gridWritten = written.grid.outcome == Outcome::Written;
            const bool waveformWritten = written.waveform == application::AnalysisImportResult::Waveform::Written;
            if (engineTookGrid(written)) {
                recordGridInLedgerAfterCommit(ctx, stickRoot, id(), key, written.grid.gridFingerprint);
                engineRowsOnNewGrid.push_back(row.id);
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

        if (cuesMove) {
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
                }
                engineSession->noteItemApplied();
                catalogs.invalidate("engine", m_enginePath.toStdString());
                said("Engine");
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
