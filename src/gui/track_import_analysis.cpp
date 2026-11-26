// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_import_analysis.hpp"

#include <QThread>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "analysis_resources.hpp"
#include "domain/audio_analysis.hpp"

#ifdef SEABASS_HAVE_QT_AUDIO
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"
#endif

namespace seabass::gui
{

int importAnalysisJobs()
{
    return analysisJobsFor(cpuCores(), QThread::idealThreadCount());
}

std::shared_ptr<const ImportAnalysis> analyseForImport(const std::string &audioFile,
                                                       const application::CancellationToken &cancel,
                                                       domain::TempoRange range)
{
#ifdef SEABASS_HAVE_QT_AUDIO
    cancel.throwIfCancelled();
    markAnalysisThread();
    // Asked before every file: whether the computer has the memory for
    // one more now. Given back when this call ends, however it ends.
    struct Turn
    {
        AnalysisMemoryGate &gate;
        int ticket;
        ~Turn() { gate.leave(ticket); }
    };
    AnalysisMemoryGate &gate = AnalysisMemoryGate::shared();
    const Turn turn{gate, gate.enter(cancel)};
    std::vector<float> samples;
    int rate = 0;
    // Held from the moment this file turns out to be a long one until its
    // analysis is done.
    static std::mutex longFiles;
    std::unique_lock<std::mutex> longTurn(longFiles, std::defer_lock);
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
            gate.noteRead(turn.ticket, static_cast<double>(samples.size()) / rate, samples.capacity() * sizeof(float));
            if (samples.size() > static_cast<std::size_t>(rate) * ImportAnalysisMaxSeconds) {
                return nullptr;
            }
            if (!longTurn.owns_lock() && samples.size() > static_cast<std::size_t>(rate) * ImportAnalysisLongSeconds) {
                while (!longTurn.try_lock()) {
                    cancel.throwIfCancelled();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
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
