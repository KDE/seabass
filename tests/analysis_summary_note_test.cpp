// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What a sync's summary says of the analysis it copied
// (gui/edit/changes/copy_analysis_change.hpp: analysisTallyRowOf(),
// AnalysisNoteCounts, analysisSummaryNote()):
//
// 1. One track, one bucket: a row the player has not analysed whose grid is
//    locked is counted as locked on the player, not also as left for the
//    player to analyse.
// 2. A row whose own analysis could not be kept first (no key: the file is
//    not under the stick root) is a bucket of its own, said as such.
// 3. A row whose flags cannot be read is said in words, not as "unknown".

#include <iostream>
#include <string>

#include "gui/edit/changes/copy_analysis_change.hpp"

using namespace seabass;
using Outcome = application::BeatGridWriteResult::Outcome;
using Waveform = application::AnalysisImportResult::Waveform;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

application::AnalysisImportResult resultOf(Outcome grid, Waveform waveform)
{
    application::AnalysisImportResult r;
    r.grid.outcome = grid;
    r.waveform = waveform;
    return r;
}

QString noteFor(const application::AnalysisImportResult &r)
{
    gui::AnalysisNoteCounts counts;
    counts.add(gui::analysisTallyRowOf(r));
    return gui::analysisSummaryNote(counts);
}

}  // namespace

int main()
{
    // 1. Unanalysed and locked: one bucket.
    {
        const auto row = gui::analysisTallyRowOf(resultOf(Outcome::RowLocked, Waveform::RowNotAnalysed));
        check(row.gridSetOnPlayer && !row.leftForPlayer, "a locked unanalysed row is counted for its lock alone");
        gui::AnalysisNoteCounts counts;
        counts.add(row);
        check(counts.setOnPlayer == 1 && counts.forPlayer == 0, "counted once");
        const QString note = gui::analysisSummaryNote(counts);
        check(note == QStringLiteral("Analysis left alone: one beat grid locked or moved on the player."),
              "the note says it once: " + note.toStdString());
    }
    // An unlocked one is still left for the player.
    {
        const auto row = gui::analysisTallyRowOf(resultOf(Outcome::RowNotAnalysed, Waveform::NoSampleCount));
        check(row.leftForPlayer && !row.gridSetOnPlayer, "an unlocked unanalysed row is left for the player");
    }

    // 2. Cannot be kept: its own bucket.
    {
        const auto r = resultOf(Outcome::CannotKeepEngines, Waveform::CannotKeepEngines);
        const auto row = gui::analysisTallyRowOf(r);
        check(row.cannotKeep && !row.grid && !row.waveform && row.otherReason.isEmpty(), "a bucket of its own");
        const QString note = noteFor(r);
        check(note.contains(QStringLiteral("one track whose own Engine analysis could not be kept first")),
              "the note says why: " + note.toStdString());
        gui::AnalysisNoteCounts two;
        two.add(row);
        two.add(row);
        check(two.cannotKeep == 2
                  && gui::analysisSummaryNote(two).contains(
                      QStringLiteral("2 tracks whose own Engine analysis could not be kept first")),
              "counted exactly");
    }

    // 3. Flags that cannot be read.
    {
        const QString note = noteFor(resultOf(Outcome::RowUnreadable, Waveform::RowUnreadable));
        check(note.contains(QStringLiteral("whether the player analysed it cannot be read")) && !note.contains("unknown"),
              "said in words: " + note.toStdString());
    }

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "analysis_summary_note_test: all checks passed\n";
    return 0;
}
