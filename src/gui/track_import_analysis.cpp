// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_import_analysis.hpp"

#include <vector>

#include "domain/audio_analysis.hpp"

#ifdef SEABASS_HAVE_QT_AUDIO
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"
#endif

namespace seabass::gui
{

std::shared_ptr<const ImportAnalysis> analyseForImport(const std::string &audioFile,
                                                       const application::CancellationToken &cancel,
                                                       domain::TempoRange range)
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
    cancel.throwIfCancelled();
    // The one call the analyser's own tools use: grid, tempo and waveform
    // from the samples, read once.
    domain::AudioAnalysis made = domain::analyseAudio(samples, rate, range);
    auto analysis = std::make_shared<ImportAnalysis>();
    analysis->waveform = std::move(made.waveform);
    if (made.grid.error.empty() && made.bpm > 0.0) {
        analysis->beats = std::move(made.grid.beats);
        analysis->bpm = made.bpm;
        analysis->confidence = made.grid.confidence;
        analysis->followsTempo = made.grid.followsTempo;
    }
    if (made.key.key) {
        analysis->key = domain::keyName(*made.key.key);
    }
    return analysis;
#else
    (void)audioFile;
    (void)cancel;
    (void)range;
    return nullptr;
#endif
}

}  // namespace seabass::gui
