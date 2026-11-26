// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_import_analysis.hpp"

#include <QThread>

#include <cstddef>
#include <span>
#include <string>

#include "analysis_resources.hpp"
#include "domain/audio_analysis.hpp"
#include "domain/pcm_source.hpp"

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
    infrastructure::audio::QtMultimediaPcmSource decoder(audioFile);
    if (decoder.sampleRate() <= 0) {
        return nullptr;
    }
    // The decoder as the analyser reads it: a cancel is asked at every
    // piece, and the gate is told how far this file has got.
    class Watched : public domain::PcmSource
    {
    public:
        Watched(domain::PcmSource &source, const application::CancellationToken &cancel, AnalysisMemoryGate &gate,
                int ticket)
            : m_source(source), m_cancel(cancel), m_gate(gate), m_ticket(ticket)
        {
        }
        int sampleRate() const override { return m_source.sampleRate(); }
        std::size_t read(std::span<float> out) override
        {
            m_cancel.throwIfCancelled();
            const std::size_t got = m_source.read(out);
            m_read += got;
            m_gate.noteRead(m_ticket, static_cast<double>(m_read) / m_source.sampleRate(), m_source.sampleRate());
            return got;
        }
        std::string error() const override { return m_source.error(); }
        std::size_t samplesRead() const { return m_read; }

    private:
        domain::PcmSource &m_source;
        const application::CancellationToken &m_cancel;
        AnalysisMemoryGate &m_gate;
        int m_ticket;
        std::size_t m_read = 0;
    };
    Watched watched(decoder, cancel, gate, turn.ticket);
    std::string decodeError;
    domain::AudioAnalysis made = domain::analyseAudio(watched, range, &decodeError);
    // A file cut short by a decode error is not the whole track.
    if (!decodeError.empty() || watched.samplesRead() == 0) {
        return nullptr;
    }
    cancel.throwIfCancelled();
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
