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
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
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
        // A .2EX the track lacks is made: noted absent, so Undo takes it
        // away. The others are only ever written over.
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

ChangeOutcome ReanalyseTrackChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string name = "\"" + subject().toStdString() + "\" (" + m_stickPath + ")";
    std::vector<std::string> done;
    const auto leftAlone = [&](const std::string &why) {
        ctx.log().record(std::string(Label) + ": " + name + ": " + why);
    };

    // Everything that can refuse is asked before the first write: the
    // files built and merged in memory, each against the file as it is
    // now. A change that fails takes the save back, so nothing of it may
    // be half on the stick.
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
        const auto same = std::find_if(merges.begin(), merges.end(), [&](const MergedFiles &m) {
            return application::normalizedPathKey(m.datPath) == application::normalizedPathKey(file.datPath);
        });
        if (same != merges.end()) {
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
            leftAlone("the analysis files at " + file.datPath + " could not be read (" + e.what() + "); "
                      + file.library.toStdString() + " left as it is");
            taken->second = false;
            continue;
        }
        const auto merged =
            infrastructure::rekordbox::mergeTrackAnlzFiles(old, built.files->dat, built.files->ext, built.files->twoEx);
        if (!merged) {
            leftAlone(file.datPath + " is not on the stick; " + file.library.toStdString() + " left as it is");
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

    try {
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

        if (toDeviceLibrary) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, m_itemCountHint, Label);
            infrastructure::rekordbox::PdbRowWriter rows(exportPdbUnder(session.writeRoot()));
            const std::vector<std::uint32_t> ids = rows.trackIdsWithFilePath(m_stickPath);
            for (const std::uint32_t id : ids) {
                rows.setTrackTempoAndKey(id, bpm, m_analysedKey);
            }
            if (!ids.empty()) {
                if (!rows.commit()) {
                    return ChangeOutcome::failure(QStringLiteral("Could not write export.pdb for %1.").arg(subject()));
                }
                session.noteItemApplied();
                done.push_back("export.pdb: tempo" + std::string(m_analysedKey.empty() ? "" : " and key " + m_analysedKey)
                               + " of " + std::to_string(ids.size()) + " row(s)");
            } else {
                leftAlone("export.pdb has no row for it any more");
            }
        }
        if (toOneLibrary) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
            try {
                const int rows = sharedOneLibraryWriter(ctx, pioneer).setTempoAndKeyForPath(m_audioFile, bpm, m_analysedKey);
                done.push_back("exportLibrary.db: tempo"
                               + std::string(m_analysedKey.empty() ? "" : " and key " + m_analysedKey) + " of "
                               + std::to_string(rows) + " row(s)");
            } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                leftAlone("exportLibrary.db has no row for it any more");
            }
        }

        std::vector<std::int64_t> engineRowsOnNewGrid;
        if (hasEngine) {
            FormatWriteSession &session =
                sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, Label);
            const std::vector<std::int64_t> rows =
                infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(), m_audioFile);
            const std::string stickRoot = ledgerStickRoot();
            const std::string key = ledgerKey();
            const auto &ledger = saveBeatGridLedger(ctx, stickRoot);
            for (const std::int64_t row : rows) {
                application::RekordboxAnalysis analysis;
                analysis.beats = built.beats;
                analysis.threeBandPreview = built.files->threeBandPreview;
                analysis.colourPreview = built.files->colourPreview;
                analysis.scrollEntries = built.files->scrollEntries;
                if (bpm > 0.0) {
                    analysis.bpm = bpm;
                }
                if (!m_analysedKey.empty()) {
                    if (const auto parsed = infrastructure::engine::parseRekordboxKey(m_analysedKey)) {
                        analysis.key = static_cast<int>(*parsed);
                    }
                }
                application::AnalysisImportOptions options;
                options.fileSampleRateHz = static_cast<double>(m_analysis->waveform.sampleRateHz);
                options.fileIsMp3 = built.isMp3;
                options.fileSampleCount = built.sampleCount;
                options.fileCountRequired = !built.isMp3;
                options.tempoWithGrid = true;
                options.keepKey = key;
                if (const auto own = ledger.find(key); !key.empty() && own != ledger.end()) {
                    options.ownGridFingerprint = own->second;
                }
                const application::AnalysisImportResult written =
                    engineAnalysisWriterFor(ctx, session, stickRoot).importAnalysis(std::to_string(row), analysis, options);
                using Outcome = application::BeatGridWriteResult::Outcome;
                if (written.grid.outcome == Outcome::Written || written.grid.outcome == Outcome::AlreadySame) {
                    recordGridInLedgerAfterCommit(ctx, stickRoot, id(), key, written.grid.gridFingerprint);
                    engineRowsOnNewGrid.push_back(row);
                }
                if (written.grid.outcome == Outcome::Written
                    || written.waveform == application::AnalysisImportResult::Waveform::Written) {
                    session.noteItemApplied();
                }
                done.push_back("Engine id " + std::to_string(row) + ": " + gridInWords(written.grid.outcome)
                               + (written.completedRow ? ", row completed" : "")
                               + (written.keptEngineAnalysis ? ", the player's analysis kept first" : ""));
            }
            if (rows.empty()) {
                leftAlone("Engine has no row for it");
            }
        }

        if (moveCues() && m_cueReview.plan.verdict == application::CueGridPlan::Verdict::Moves) {
            const QString failed = moveCuesOntoNewGrid(ctx, toDeviceLibrary, toOneLibrary, engineRowsOnNewGrid, done);
            if (!failed.isEmpty()) {
                return ChangeOutcome::failure(failed);
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }

    if (done.empty()) {
        leftAlone("left as it is: no library on the stick had an analysis of it to replace");
        return ChangeOutcome::skip();
    }
    std::string line;
    for (const std::string &part : done) {
        line += (line.empty() ? "" : "; ") + part;
    }
    ctx.log().record(std::string(Label) + ": analysed " + name + " again, " + QString::number(bpm, 'f', 2).toStdString() + " BPM"
                     + (m_analysedKey.empty() ? std::string(", key left as it is") : ", key " + m_analysedKey) + ": "
                     + line);
    return ChangeOutcome::success();
}

QString ReanalyseTrackChange::moveCuesOntoNewGrid(SaveContext &ctx, bool toDeviceLibrary, bool toOneLibrary,
                                                 const std::vector<std::int64_t> &engineRows,
                                                 std::vector<std::string> &done)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string fileKey = application::normalizedPathKey(m_audioFile);
    // The library's cues as they are now, planned again against the two
    // grids the review showed: what another change of this save did to
    // them counts, and a cue that is no longer on a beat stays.
    const auto plannedFor = [&](const domain::Track &track) {
        return application::planCueGridMoves(track.cues, m_cueReview.oldBeats, m_cueReview.newBeats);
    };
    const auto said = [&](const char *library, const application::CueGridPlan &plan) {
        for (const application::CueGridMove &move : plan.moves) {
            std::string line = std::string(library) + ": moved "
                               + cueLabel(move.kind, move.hotCueNumber, move.isLoop).toStdString() + " from "
                               + clockText(move.oldMs).toStdString() + " to " + clockText(move.newMs).toStdString();
            if (move.isLoop) {
                line += " (its end from " + clockText(move.oldEndMs).toStdString() + " to "
                        + clockText(move.newEndMs).toStdString() + ")";
            }
            done.push_back(line);
        }
    };
    auto &catalogs = LibraryCatalogCache::instance();
    if (toDeviceLibrary || toOneLibrary) {
        const std::string format = toDeviceLibrary ? "rekordbox" : "onelibrary";
        const std::vector<domain::Track> tracks = catalogs.tracksFor(format, pioneer, ctx.progress(), ctx.cancel());
        for (const domain::Track &track : tracks) {
            if (application::normalizedPathKey(track.filePath) != fileKey) {
                continue;
            }
            const application::CueGridPlan plan = plannedFor(track);
            if (plan.verdict != application::CueGridPlan::Verdict::Moves) {
                continue;
            }
            const std::vector<domain::CuePoint> moved = application::cuesMovedOntoNewGrid(track.cues, plan);
            if (toDeviceLibrary) {
                // The analysis files are declared in filesToBackup(); the
                // writer reads them as this change just wrote them.
                infrastructure::rekordbox::RekordboxCueWriter writer(pioneer, sharedAnlzPathIndex(ctx, m_pioneerRoot));
                writer.writeHotCues(track.sourceId, moved);
                if (toOneLibrary) {
                    const QString failed = mirrorCuesOrExplain(sharedOneLibraryWriter(ctx, pioneer), m_audioFile, moved,
                                                               ctx, Label, QStringLiteral("move the cue points"));
                    if (!failed.isEmpty()) {
                        return failed;
                    }
                }
            } else {
                sharedOneLibraryWriter(ctx, pioneer).writeCuesForPath(m_audioFile, moved);
            }
            said(toDeviceLibrary ? (toOneLibrary ? "DeviceLibrary and OneLibrary" : "DeviceLibrary") : "OneLibrary", plan);
            break;
        }
    }
    if (!engineRows.empty()) {
        FormatWriteSession &session =
            sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, Label);
        const std::vector<domain::Track> tracks =
            catalogs.tracksFor("engine", m_enginePath.toStdString(), ctx.progress(), ctx.cancel());
        for (const std::int64_t row : engineRows) {
            for (const domain::Track &track : tracks) {
                if (track.sourceId != std::to_string(row)) {
                    continue;
                }
                const application::CueGridPlan plan = plannedFor(track);
                if (plan.verdict == application::CueGridPlan::Verdict::Moves) {
                    engineAnalysisWriterFor(ctx, session, ledgerStickRoot())
                        .writeHotCues(track.sourceId, application::cuesMovedOntoNewGrid(track.cues, plan));
                    session.noteItemApplied();
                    said("Engine", plan);
                }
                break;
            }
        }
    }
    return {};
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
