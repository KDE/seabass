// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/use_cases/copy_beat_grids.hpp"
#include "infrastructure/local/cached_sample_rates.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/exact_sample_count.hpp"
#endif

namespace seabass::infrastructure::local
{

// The facts Beat Grids & Waveforms needs of an item's audio file to
// complete a row the player has not analysed (application::AnalysisFileFacts),
// the same for the page and the command line: the file's own sample count
// and the rate read with it wherever its container says them exactly
// (audio::readExactSampleCount(): FLAC, WAV, AIFF, M4A, an MP3's frames).
// Otherwise its rate through the stick's cache; without TagLib in the
// build a FLAC, WAV, AIFF or M4A then gives no count and is not completed.
inline application::AnalysisFileFacts analysisFileFactsOf(const application::BeatGridCopyItem &item,
                                                          CachedSampleRates &rates)
{
    application::AnalysisFileFacts f;
    if (item.filePath.empty()) {
        return f;
    }
    f.isMp3 = application::isMp3File(item.filePath);
    f.countRequired = application::requiresOwnSampleCount(item.filePath);
#ifdef SEABASS_HAVE_TAGLIB
    {
        if (const auto exact = audio::readExactSampleCount(item.filePath)) {
            f.sampleCount = exact->samples;
            f.sampleRateHz = exact->sampleRateHz;
            return f;
        }
    }
#endif
    f.sampleRateHz = rates.rateOf(item.filePath);
    return f;
}

}  // namespace seabass::infrastructure::local
