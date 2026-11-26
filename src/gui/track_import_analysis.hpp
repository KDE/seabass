// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "domain/seabass_grid_analyser.hpp"
#include "gui/edit/changes/import_track_change.hpp"

namespace seabass::gui
{

// Seabass's own analysis of an audio file for the track import, a test
// version: the file is decoded once (QtMultimediaPcmSource, whole into
// memory: about 10 MB a minute) and given to domain::analyseAudio,
// which makes grid, tempo and waveform. Null when the file cannot be
// decoded, or this build has no decoder. A grid the analyser could not
// make leaves the waveform alone in the result.
//
// A file longer than ImportAnalysisMaxSeconds is not analysed: it is
// held in memory whole, and the analyser's own working memory comes on
// top (measured: a 65 minute mix takes 2.8 GB and 1 min 42 s on one
// core). An hour and a half covers a recorded mix; two of them analysed
// at once are about 8 GB.
//
// Needs a QCoreApplication; runs a nested event loop in the calling
// thread. Throws application::OperationCancelled on a cancel, which is
// asked between the steps, not inside them.
constexpr int ImportAnalysisMaxSeconds = 90 * 60;
// From this length on a file is a long one, and only one long file is
// analysed at a time, however many are analysed at once: the others wait
// their turn, so that several mixes do not take several GB each together.
constexpr int ImportAnalysisLongSeconds = 20 * 60;

// How many files an import analyses at once on this computer
// (analysisJobsFor: by its cores and their strength, two at least, eight
// at most). Whether the next file may start is asked of the memory that
// is free at that moment (AnalysisMemoryGate), file by file.
int importAnalysisJobs();

//
// `range`: the tempos a grid may have (domain::TempoRange); the default
// until Preferences has a setting for it.
std::shared_ptr<const ImportAnalysis> analyseForImport(const std::string &audioFile,
                                                       const application::CancellationToken &cancel,
                                                       domain::TempoRange range = {});

}  // namespace seabass::gui
