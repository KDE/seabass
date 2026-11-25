// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_import_analysis.hpp"

#include <algorithm>
#include <vector>

#include "domain/pcm_source.hpp"
#include "domain/seabass_grid_analyser.hpp"
#include "domain/waveform_from_audio.hpp"

#ifdef SEABASS_HAVE_QT_AUDIO
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"
#endif

namespace seabass::gui
{

std::shared_ptr<const ImportAnalysis> analyseForImport(const std::string &audioFile,
                                                       const application::CancellationToken &cancel)
{
#ifdef SEABASS_HAVE_QT_AUDIO
    cancel.throwIfCancelled();
    std::vector<float> samples;
    int rate = 0;
    {
        infrastructure::audio::QtMultimediaPcmSource decoder(audioFile);
        rate = decoder.sampleRate();
        if (rate <= 0) {
            return nullptr;
        }
        std::vector<float> chunk(1 << 16);
        for (;;) {
            cancel.throwIfCancelled();
            const std::size_t got = decoder.read(chunk);
            if (got == 0) {
                break;
            }
            samples.insert(samples.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
            if (samples.size() > static_cast<std::size_t>(rate) * ImportAnalysisMaxSeconds) {
                return nullptr;
            }
        }
        // A file cut short by a decode error is not the whole track.
        if (!decoder.error().empty() || samples.empty()) {
            return nullptr;
        }
    }
    auto analysis = std::make_shared<ImportAnalysis>();
    analysis->waveform = domain::waveformFromSamples(samples, rate);
    cancel.throwIfCancelled();

    domain::InMemoryPcmSource source(std::move(samples), rate);
    domain::GridAnalysis grid = domain::SeabassGridAnalyser().analyse(source);
    if (grid.error.empty() && grid.beats.size() >= 2) {
        std::vector<double> gaps;
        for (std::size_t i = 1; i < grid.beats.size(); ++i) {
            gaps.push_back(grid.beats[i].timeMs - grid.beats[i - 1].timeMs);
        }
        std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2), gaps.end());
        const double gap = gaps[gaps.size() / 2];
        if (gap > 0.0) {
            analysis->bpm = 60000.0 / gap;
            analysis->beats = std::move(grid.beats);
            analysis->confidence = grid.confidence;
        }
    }
    return analysis;
#else
    (void)audioFile;
    (void)cancel;
    return nullptr;
#endif
}

}  // namespace seabass::gui
