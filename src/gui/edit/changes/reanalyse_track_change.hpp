// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "application/use_cases/carry_cues_to_new_grid.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Browse's Analyse again, one track: Seabass's own analysis of the audio
// file as it is on the stick (made before the save, ImportAnalysis),
// written over the analysis every library on the stick has of that file.
// Staged per track by TrackImportController::analyseAgain() for the
// tracks of a playlist; nothing is written until the save.
//
// rekordbox's two libraries share the track's analysis files (.DAT, .EXT,
// .2EX) through the file their rows name. In each, the grid and the
// waveforms are replaced and everything else is kept byte for byte, the
// cue lists first of all (infrastructure::rekordbox::mergeTrackAnlzFiles);
// a .2EX the track lacks is written whole. Then export.pdb's row gets the
// grid's tempo (PdbRowWriter::setTrackTempoAndKey) and exportLibrary.db's
// rows the same (OneLibraryCueWriter::setTempoAndKeyForPath). A library
// whose rows name no analysis file, or one that is not on the stick, is
// left alone, rows and all: a tempo beside an analysis it does not belong
// to would be two analyses in one row.
//
// Engine's row gets grid, overview and tempo through the analysis writer
// the import and Beat Grids & Waveforms use, by their rules: a row the
// player has not analysed is completed; on one it has, a grid locked or
// moved on the player is left alone (and so is its tempo), and the
// player's own analysis is kept on the stick before it is first replaced.
// The grid written is recorded in the beat grid ledger, so the next
// Analyse again, or a copy, knows it for Seabass's.
//
// The key: the analyser's, in rekordbox's spelling, goes into the rows
// only where the file's tags name no key (`analysedKey` empty means the
// rows' key stays). Engine takes it only on a row it completes: on an
// analysed row the key lives in the analysis record, which the kept
// analysis is recognised by, and writing it there would have the next
// write take Seabass's key for a new analysis of the player's.
//
// Cues and loops on a beat of the old grid can follow the new one
// (application::planCueGridMoves()), only where the person asked for it
// for this track (setMoveCues(), off until then, settable until the
// save). At the save each library's cues are read as they are then and
// planned again against the same two grids, and those that move are
// written through that library's own cue writer (rekordbox's analysis
// files with OneLibrary kept in step, Engine's row), the whole set as
// every cue change writes it. Engine's cues follow only where Engine's
// row took the new grid: next to a grid left alone (locked, moved on the
// player) they belong to that grid.
//
// The change is tied to the track's file (unitKeys(), id()): staging the
// track again replaces it. One unit of the save; a track whose libraries
// all turn out to have nothing to replace when the save reaches it is a
// skip, said in the log.
class ReanalyseTrackChange : public PendingChange
{
public:
    // `audioFile`: the file on the stick, absolute (UTF-8); `stickPath`:
    // its place from the stick's root, "/Contents/...". `analysisFiles`:
    // the .DAT paths the rows of rekordbox's libraries name, as they spell
    // them ("/PIONEER/USBANLZ/.../ANLZ0000.DAT"), with the library that
    // names each ("rekordbox", "onelibrary"). `title`/`artist` for the
    // log and the list of edits.
    struct AnalysisFileOf
    {
        QString library;
        std::string datPath;
    };
    ReanalyseTrackChange(std::string audioFile, std::string stickPath, QString stickRoot, QString pioneerRoot,
                         QString enginePath, std::vector<AnalysisFileOf> analysisFiles,
                         std::shared_ptr<const ImportAnalysis> analysis, std::string analysedKey, QString title,
                         QString artist, int itemCountHint);

    // A time as the review and the log show it, "m:ss.mmm" ("-" before a
    // time before the track's start), and a cue by its kind and pad
    // ("Hot cue 2", "Hot loop 4", "Memory cue", "Memory loop").
    static QString clockText(double ms);
    static QString cueLabel(domain::CuePoint::Kind kind, int hotCueNumber, bool isLoop);

    static QString idFor(const std::string &stickPath);
    static QString unitKeyFor(const std::string &stickPath);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList unitKeys() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

    // What the review shows of the track's cues, made when the analysis
    // was: both grids and the cues on rekordbox's timeline (where every
    // library's cues are read), and the plan for them. The analysis's own
    // waveform is on the decoder's timeline; `waveformOffsetMs` moves it
    // onto rekordbox's (infrastructure::audio::cueTimelineOffsetMs()).
    struct CueReview
    {
        std::vector<double> oldBeats;
        std::vector<double> newBeats;
        std::vector<domain::CuePoint> cues;
        application::CueGridPlan plan;
        double waveformOffsetMs = 0.0;
        double oldBpm = 0.0;
    };
    void setCueReview(CueReview review) { m_cueReview = std::move(review); }
    const CueReview &cueReview() const { return m_cueReview; }
    // Whether the save moves the cues that sit on the old grid; off by
    // default. Set on the GUI thread while the change waits, read by the
    // save's.
    void setMoveCues(bool move) { m_moveCues.store(move); }
    bool moveCues() const { return m_moveCues.load(); }

    // The review's picture of one planned move (TrackImportController::
    // cueReview): {spanMs, columns: [{low, mid, high}], oldBeats, newBeats,
    // oldCueMs, newCueMs, and a loop's oldEndMs, newEndMs}, the new
    // analysis's waveform a few beats either side of the cue. Everything
    // on rekordbox's timeline, the waveform moved there by
    // `waveformOffsetMs`, and given in ms from the strip's first column,
    // so the view only draws.
    QVariantMap reviewStrip(const application::CueGridMove &move) const;
    // How far the grid moved, as one number: the middle of the moves of
    // the beats both grids have.
    double gridShiftMs() const;

    const std::string &audioFile() const { return m_audioFile; }
    const std::shared_ptr<const ImportAnalysis> &analysis() const { return m_analysis; }
    const std::string &analysedKey() const { return m_analysedKey; }

private:
    // The three files of a .DAT path, absolute, on the stick.
    std::vector<std::string> anlzFilesOf(const std::string &datPath) const;
    // The stick root as Beat Grids & Waveforms keys its ledger and kept
    // analyses.
    std::string ledgerStickRoot() const;
    std::string ledgerKey() const;
    // The cues on the old grid moved onto the new one in each library
    // that took the new analysis (see the class comment); a line for the
    // log per cue moved into `done`. A message when the change must fail.
    QString moveCuesOntoNewGrid(SaveContext &ctx, bool toDeviceLibrary, bool toOneLibrary,
                                const std::vector<std::int64_t> &engineRows, std::vector<std::string> &done);

    std::string m_audioFile;
    std::string m_stickPath;
    QString m_stickRoot;
    QString m_pioneerRoot;
    QString m_enginePath;
    std::vector<AnalysisFileOf> m_analysisFiles;
    std::shared_ptr<const ImportAnalysis> m_analysis;
    std::string m_analysedKey;
    QString m_title;
    QString m_artist;
    int m_itemCountHint = 1;
    CueReview m_cueReview;
    std::atomic<bool> m_moveCues{false};
};

}  // namespace seabass::gui
