// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "application/use_cases/carry_cues_to_new_grid.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Browse's Restore original analysis, one track: the analysis rekordbox
// and the player made of it, kept on the stick when Analyse again first
// replaced it, put back in every library that has one kept. Staged per
// track by OriginalAnalysisController for the tracks of a playlist;
// nothing is written until the save.
//
// rekordbox's two libraries: the kept analysis files
// (local/kept_rekordbox_analysis_file.hpp) go back by
// rekordbox::restoreAnlzAnalysis(): every section the original had, as it
// had it, with the cue lists of the files as they are now; a .2EX that
// Analyse again made where the track had none is taken away again. Then
// every row that was kept gets its tempo and key back as stored. Engine:
// the player's own analysis kept by the analysis writer goes back by
// LibdjinteropEngineCueWriter::restoreEngineAnalysis(), the beat grid
// ledger's line for the track taken out after the commit.
//
// Left alone whole, nothing written, a plain reason in the log and the
// save's summary, when:
// - the audio file is not the one the analysis was kept for (another
//   size, or not there: the size decides, as the analysis ledger's
//   classifyAnalysis() decides it);
// - the kept files do not belong to the track any more: its rows name
//   other analysis files now, or the .DAT names another audio file than
//   the kept one did (an export reused the folder);
// - the grid in the track's .DAT is neither one Seabass wrote (the
//   analysis ledger's fingerprint, or one the kept entry records) nor the
//   kept original's: rekordbox or a player changed it since, and that is
//   never written over;
// - Engine's row changed since Seabass wrote it (the player analysed it
//   again, or the DJ moved or locked the grid), or what is kept of it is
//   damaged or of another row;
// - what is kept cannot be read.
// Everything that can refuse is asked before the first write.
//
// Cue points that Analyse again moved with the grid would sit off the
// grid put back. The same reviewed, all or nothing per track move is
// offered the other way (setMoveCues(), setMoveCuesByThreshold(), as
// ReanalyseTrackChange's): old grid Seabass's (the .DAT's now), new grid
// the kept original's. They move only where every library that holds
// cues for the track holds the ones the review showed, and every library
// holding them takes the grid back; otherwise no cue moves anywhere, the
// analysis still goes back, and the log and the summary say why. A track
// with no kept rekordbox analysis (Engine alone) offers no cue move: the
// review has no grid on rekordbox's timeline to plan it with.
//
// Every file written is read back and compared before it is believed.
// After the save landed: the analysis ledger's entry for the track is
// taken out when every library that held Seabass's analysis got its
// original back, or loses the grid of the library that did; the kept
// entries are taken away in the save itself, backed up first, so Undo
// Last Save puts them back with the analysis. Restoring twice is safe:
// the second finds nothing kept and leaves the track alone.
class RestoreAnalysisChange : public PendingChange
{
public:
    // `audioFile`: the file on the stick, absolute (UTF-8); `stickPath`:
    // its place from the stick's root, "/Contents/...". `keptDatPaths`:
    // the .DAT paths of the kept rekordbox analysis, as the rows spell
    // them, when there is one; `engineKept`: the player's analysis of an
    // Engine row of the file is kept. `title`/`artist` for the log.
    RestoreAnalysisChange(std::string audioFile, std::string stickPath, QString stickRoot, QString pioneerRoot,
                          QString enginePath, std::vector<std::string> keptDatPaths, bool engineKept, QString title,
                          QString artist, int itemCountHint);

    static QString idFor(const std::string &stickPath);

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
    std::vector<RekordboxWrite> rekordboxWrites() const override;

    // What the review shows: the grid Seabass left (old), the kept one
    // (new), both on rekordbox's timeline, the track's cues and the plan.
    struct CueReview
    {
        std::vector<double> oldBeats;
        std::vector<double> newBeats;
        std::vector<domain::CuePoint> cues;
        application::CueGridPlan plan;
        double oldBpm = 0.0;
    };
    void setCueReview(CueReview review) { m_cueReview = std::move(review); }
    const CueReview &cueReview() const { return m_cueReview; }
    // As ReanalyseTrackChange's switch: off by default; the person's
    // choice stays theirs; the threshold rule's otherwise.
    void setMoveCues(bool move) { m_cueSwitch.store(move ? OnByPerson : OffByPerson); }
    void setMoveCuesByThreshold(bool follows, int thresholdMs)
    {
        m_cueSwitch.store(follows && thresholdMs > 0 ? thresholdMs : OffByRule);
    }
    bool moveCues() const { return isOn(m_cueSwitch.load()); }
    bool moveCuesChosen() const
    {
        const int now = m_cueSwitch.load();
        return now == OnByPerson || now == OffByPerson;
    }
    int movesOnTheirOwnUnderMs() const { return std::max(m_cueSwitch.load(), 0); }

    const std::string &audioFile() const { return m_audioFile; }
    const std::string &stickPath() const { return m_stickPath; }
    bool restoresRekordbox() const { return !m_keptDatPaths.empty(); }
    bool restoresEngine() const { return m_engineKept; }

private:
    std::vector<std::string> anlzFilesOf(const std::string &datPath) const;
    std::string ledgerStickRoot() const;
    std::string ledgerKey() const;

    std::string m_audioFile;
    std::string m_stickPath;
    QString m_stickRoot;
    QString m_pioneerRoot;
    QString m_enginePath;
    std::vector<std::string> m_keptDatPaths;
    bool m_engineKept = false;
    QString m_title;
    QString m_artist;
    int m_itemCountHint = 1;
    CueReview m_cueReview;
    static constexpr int OffByRule = 0;
    static constexpr int OnByPerson = -1;
    static constexpr int OffByPerson = -2;
    static bool isOn(int cueSwitch) { return cueSwitch > 0 || cueSwitch == OnByPerson; }
    std::atomic<int> m_cueSwitch{OffByRule};
    std::optional<RekordboxWrite> m_written;
};

}  // namespace seabass::gui
