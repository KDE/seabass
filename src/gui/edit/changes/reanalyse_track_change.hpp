// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
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
// waveforms are replaced and everything else is kept byte for byte
// (infrastructure::rekordbox::mergeTrackAnlzFiles), the cue lists too
// unless the person asked for the cues to follow the grid (below); a
// .2EX the track lacks is written whole. Then export.pdb's rows get the
// grid's tempo (PdbRowWriter::setTrackTempoAndKey) and exportLibrary.db's
// rows the same (OneLibraryCueWriter::setTempoAndKeyForPath). A library
// whose rows name no analysis file, or one that is not on the stick, is
// left alone, rows and all: a tempo beside an analysis it does not belong
// to would be two analyses in one row.
//
// Engine's row gets grid, overview and tempo through the analysis writer
// the import and Beat Grids & Waveforms use, by their rules: a row the
// player has not analysed is completed; on one it has, a row whose grid
// is locked or moved on the player is left alone whole (grid, overview
// and tempo: a new overview under the DJ's grid would be half an
// analysis), and the player's own analysis is kept on the stick before it
// is first replaced.
// The grid written is recorded in the beat grid ledger, so the next
// Analyse again, or a copy, knows it for Seabass's.
//
// The key: a key a row has is never replaced. It is rekordbox's analysis
// or the DJ's choice, and the analyser's is a guess. The analyser's key
// (`analysedKey`, rekordbox's spelling) goes only into a row that names
// none, in each library whose writer can take it safely: export.pdb and
// exportLibrary.db, and an Engine row the writer completes. Not an Engine
// row the player analysed: there the key lives in the analysis record,
// which the kept analysis is recognised by, and writing it would have the
// next write take Seabass's key for a new analysis of the player's.
//
// Cues and loops on a beat of the old grid can follow the new one
// (application::planCueGridMoves()), only where the person asked for it
// for this track (setMoveCues(), off until then, settable until the
// save), or where every move is under the threshold the person set for
// moves that need no asking (setMoveCuesOnTheirOwn(), which the review
// shows and the person can switch off). At the save, before anything is
// written, every library's cues for the file are read as they are then,
// for this file alone (the
// analysis files of its rows, Engine's rows from the save's own copy of
// m.db at the file's own sample rate), never the whole catalog and planned against the two grids the review showed, each
// library for the kinds it holds: rekordbox's two exactly as the review
// (cuesMatchTheReview()), Engine by its pads, with its one main cue
// following a memory cue the review moves and otherwise staying
// (engineCuesOntoNewGrid()). The cues move only if every library that
// holds cues for the track agrees, each library's rows hold one set and
// DeviceLibrary's is OneLibrary's (a cue only one of them holds would be
// lost to the other's set), and every library holding cues takes the new
// grid; otherwise no cue of the track
// moves anywhere, the grid part still applies, and the log and the
// save's summary say why. A track whose rows name an analysis file that
// was not the track's when it was analysed (so not declared for backup)
// is left alone whole.
//
// Every grid of every library is written before any cue. An Engine row
// the plan moved cues on that does not take the new grid when it is
// written fails the change, and the save goes back whole: the others'
// grids are written by then, and its cues would stay on the old one.
// Those that move are written through each library's own cue writer
// (rekordbox's analysis files of every row with OneLibrary kept in step,
// Engine's rows), the whole set as every cue change writes it, and
// reported as Seabass's write for the rekordbox baseline
// (rekordboxWrites()).
//
// After each library's write the catalog cache is told, so a cue change
// later in the same save reads the track as this one left it.
//
// The change is tied to the track's file (unitKeys(), id()): staging the
// track again replaces it. One unit of the save. A track where no library
// took anything (all refused, or all the same already) is a skip, its
// reason in the log; one where some did and a library refused is a
// success whose summary note names the library left as it was.
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
    // The cue set of the track's rekordbox side, as the last apply() wrote
    // it, when it moved cues. Not the grid, tempo and key: the rekordbox
    // baseline has nothing to record them in (RekordboxWrite holds cues
    // and a rating only).
    std::vector<RekordboxWrite> rekordboxWrites() const override;

    // Whether `plan` holds exactly `review`'s moves: the same number, each
    // of the review's matched by one of the same kind, pad (or memory) and
    // loop-ness, on the same beat, its old and new places (a loop's ends
    // too) within `toleranceMs` (domain::cueToleranceMsFor() of the
    // track's tempo, the app's cue comparison). And a plan that moves.
    static bool cuesMatchTheReview(const application::CueGridPlan &review, const application::CueGridPlan &plan,
                                   double toleranceMs);

    // Engine's cues moved onto the new grid, compared with the review for
    // the kinds Engine holds: its pads (hot cues and hot loops) must move
    // exactly as the review moves the same pads, and a pad the review
    // does not move by number may only be a memory cue (or loop) the
    // review moves from the same place, as Engine DJ's import puts those
    // on free pads. Engine's one main cue follows a memory cue the review
    // moves from its place, and otherwise stays where it is without
    // refusing. nullopt when Engine holds other cue points than the
    // review showed. The cues as given when nothing of Engine's moves.
    static std::optional<std::vector<domain::CuePoint>> engineCuesOntoNewGrid(const application::CueGridPlan &review,
                                                                             const std::vector<domain::CuePoint> &cues,
                                                                             const std::vector<double> &oldBeats,
                                                                             const std::vector<double> &newBeats,
                                                                             double toleranceMs);

    // Test seam: called at each step of apply() named here, with the step
    // ("engine-grid:<id>" before an Engine row's grid write,
    // "rekordbox-cues:<id>" and "engine-cues:<id>" after a row's cue
    // write), so a test can change the stick between a plan and its
    // write, or fail the change in the middle. Never set in the app.
    using StepHook = std::function<void(const std::string &step)>;
    static void setStepHookForTesting(StepHook hook);

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
    // save's. setMoveCues() is the person's choice for this track;
    // setMoveCuesOnTheirOwn() turns the move on by the threshold rule
    // (application::cuesFollowOnTheirOwn() under `thresholdMs`), which the
    // save's log says, so a move nobody asked for one by one is named as
    // such. A choice made afterwards takes the rule's place.
    void setMoveCues(bool move)
    {
        m_onTheirOwnUnderMs.store(0);
        m_moveCues.store(move);
    }
    void setMoveCuesOnTheirOwn(int thresholdMs)
    {
        m_onTheirOwnUnderMs.store(thresholdMs);
        m_moveCues.store(true);
    }
    bool moveCues() const { return m_moveCues.load(); }
    // The threshold the cues follow on their own under; 0 when they move
    // (or stay) by the person's choice.
    int movesOnTheirOwnUnderMs() const { return m_moveCues.load() ? m_onTheirOwnUnderMs.load() : 0; }

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
    std::atomic<int> m_onTheirOwnUnderMs{0};
    std::optional<RekordboxWrite> m_written;
};

}  // namespace seabass::gui
