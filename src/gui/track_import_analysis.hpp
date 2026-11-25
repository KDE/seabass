// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "gui/edit/changes/import_track_change.hpp"

namespace seabass::gui
{

// Seabass's own analysis of an audio file for the track import, a test
// version: the file is decoded once (QtMultimediaPcmSource, whole into
// memory: about 10 MB a minute), the beat grid is
// domain::SeabassGridAnalyser's with its default tempo range, the
// waveform domain::waveformFromSamples'. Null when the file cannot be
// decoded, or this build has no decoder. A grid the analyser could not
// make leaves the waveform alone in the result.
//
// A file longer than ImportAnalysisMaxSeconds (a recorded set) is not
// analysed: it would be held in memory whole.
//
// Needs a QCoreApplication; runs a nested event loop in the calling
// thread. Throws application::OperationCancelled on a cancel, which is
// asked between the steps, not inside them.
constexpr int ImportAnalysisMaxSeconds = 20 * 60;

std::shared_ptr<const ImportAnalysis> analyseForImport(const std::string &audioFile,
                                                       const application::CancellationToken &cancel);

}  // namespace seabass::gui
