// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/copy_beat_grids.hpp"

namespace seabass::gui
{

// Result of the background creation task, see
// EngineLibraryCreatorController::create(). Built entirely on a worker
// thread, no access to the controller.
struct EngineLibraryCreationTaskResult
{
    int tracksCreated = 0;
    int tracksSkipped = 0;
    int cuesCopied = 0;
    int tracksTotal = 0;
    bool cancelled = false;  // nothing was created on the stick
    QString errorMessage;  // empty on success
    // The library was made from a rekordbox export but could not be told
    // so (issue #42): a player may offer to import over it on first insert.
    bool importNotRecorded = false;
    // Cues the MP3 lead-in shift left out (#50,
    // EngineLibraryCreationResult): said, not lost quietly.
    int memoryCuesDroppedByLeadInShift = 0;
    int loopsDroppedByLeadInShift = 0;
    // With "Copy beat grids/waveforms from rekordbox to Engine" on in
    // Preferences: what the copy of rekordbox's analysis did, in the
    // sentence the syncs say (analysisSummaryNote()); empty when the
    // switches are off or nothing was created.
    QString analysisNote;
    // The ledger of the grids Seabass wrote could not be written after the
    // library was: said, since a later copy then leaves those grids alone.
    QString analysisWarning;
    // What the note was made from, for tests.
    int analysisGrids = 0;
    int analysisWaveforms = 0;
    int analysisCompleted = 0;
    int analysisLeftForPlayer = 0;
    int analysisLeftOther = 0;
};

// Create Engine Library, start to end, on the calling (worker) thread: the
// rekordbox export at `rekordboxPath` read through the catalog cache, the
// library built in a scratch folder (EngineLibraryCreator), then copied to
// "Engine Library" beside PIONEER. One bar on `reporter` for all of it.
//
// `analysis`: Preferences' "Copy beat grids/waveforms from rekordbox to
// Engine" (application::SyncAnalysisPolicy, read by the caller on the GUI
// thread). Every row is new and unanalysed, so a row is completed only
// with both: rekordbox's grid, its waveform (overview and overview file),
// the analysis record over the file's own sample count and the analysed
// flag, in the scratch library, before anything reaches the stick (a write
// failure there fails the creation, a cancel cancels it, the stick
// untouched either way). A row that cannot be completed (no rekordbox
// preview, a file whose length cannot be read, rekordbox's analysis
// unreadable) is left for the player as before and counted. One switch
// alone completes nothing: every row is left for the player, and the note
// says why. Once the library is on the stick the grids written are put in
// the stick's ledger, so a later copy knows them for Seabass's own. With
// both off the library is the one this always made.
EngineLibraryCreationTaskResult runEngineLibraryCreation(const QString &rekordboxPath, int schemaGeneration,
                                                         const application::AnalysisCopyOptions &analysis,
                                                         application::ProgressReporter &reporter,
                                                         application::CancellationToken cancel);

}  // namespace seabass::gui
