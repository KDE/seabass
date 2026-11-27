// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "domain/pcm_source.hpp"
#include "domain/seabass_grid_analyser.hpp"
#include "gui/edit/changes/import_track_change.hpp"

namespace seabass::gui
{

class AnalysisMemoryGate;

// How many files an import analyses at once on this computer
// (analysisJobsFor: by its cores and their strength, two at least, eight
// at most). Whether the next file may start is asked of the memory that
// is free at that moment (AnalysisMemoryGate), file by file.
int importAnalysisJobs();

// Seabass's own analysis of an audio file for the track import: the file
// is decoded once (QtMultimediaPcmSource) and given to
// domain::analyseAudio piece by piece as it is decoded, which makes
// grid, tempo, key and waveform. The track is never held whole, so a
// recorded mix of some hours is analysed like any track: 50 MB and
// 3.5 MB a minute (analysis_resources.hpp). Null when the file cannot be
// decoded, was cut short by a decode error, or this build has no
// decoder, or went on decoding past ImportAnalysisMaxSeconds. A grid the
// analyser could not make leaves the waveform alone in the result.
//
// Needs a QCoreApplication; runs a nested event loop in the calling
// thread. Throws application::OperationCancelled on a cancel, which is
// asked at every piece read.
//
// `range`: the tempos a grid may have (domain::TempoRange). The import
// passes the one Preferences sets (storedAnalysisTempoRange()).
std::shared_ptr<const ImportAnalysis> analyseForImport(const std::string &audioFile,
                                                       const application::CancellationToken &cancel,
                                                       domain::TempoRange range = {});

// A file that goes on decoding past a day of audio is broken, not a
// recording: its analysis is given up, so it cannot run for ever.
inline constexpr double ImportAnalysisMaxSeconds = 24.0 * 3600.0;

// analyseForImport() with what it takes from the computer given: `open`
// opens the file's decoder (after the gate let the track in, and after the
// file was measured), `gate` is the memory gate, and a source read past
// `maxSeconds` is given up. The gate's ticket is given back however the
// call ends. analyseForImport() is this with QtMultimediaPcmSource, the
// shared gate and ImportAnalysisMaxSeconds; a test passes its own.
std::shared_ptr<const ImportAnalysis> analyseSourceForImport(
    const std::string &audioFile, const std::function<std::unique_ptr<domain::PcmSource>()> &open,
    const application::CancellationToken &cancel, domain::TempoRange range, AnalysisMemoryGate &gate,
    double maxSeconds = ImportAnalysisMaxSeconds);

}  // namespace seabass::gui
