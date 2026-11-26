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

// Seabass's own analysis of an audio file for the track import: the file
// is decoded once (QtMultimediaPcmSource) and given to
// domain::analyseAudio piece by piece as it is decoded, which makes
// grid, tempo, key and waveform. The track is never held whole, so a
// recorded mix of some hours is analysed like any track: 50 MB and
// 3.5 MB a minute (analysis_resources.hpp). Null when the file cannot be
// decoded, was cut short by a decode error, or this build has no
// decoder. A grid the analyser could not make leaves the waveform alone
// in the result.
//
// Needs a QCoreApplication; runs a nested event loop in the calling
// thread. Throws application::OperationCancelled on a cancel, which is
// asked at every piece read.

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
