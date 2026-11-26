// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "track_import_analysis.hpp"

#include <QThread>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <utility>
#include <string>

#include "analysis_resources.hpp"
#include "domain/audio_analysis.hpp"
#include "domain/pcm_source.hpp"

#include "infrastructure/local/analysis_ledger_file.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "musicanalyze/version.hpp"

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
    // The file as it was before the decoder opened it: an analysis gets
    // a provenance only when it is the same after the last sample.
    const auto measure = [&audioFile]() -> std::optional<std::pair<std::uint64_t, std::int64_t>> {
        std::error_code ec;
        const std::filesystem::path path = pathFromUtf8(audioFile);
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) {
            return std::nullopt;
        }
        const auto written = std::filesystem::last_write_time(path, ec);
        if (ec) {
            return std::nullopt;
        }
        return std::make_pair(static_cast<std::uint64_t>(size), infrastructure::stick_backup::toUnixSeconds(written));
    };
    const auto before = measure();
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
    if (const auto after = measure(); before && after == before) {
        const domain::TempoRange used = musicanalyze::usableTempoRange(range);
        ImportAnalysis::Provenance provenance;
        provenance.inputRevision = infrastructure::audio::AnalysisInputRevision;
        provenance.gridRevision = made.revisions.grid;
        provenance.keyRevision = made.revisions.key;
        provenance.waveformRevision = made.revisions.waveform;
        provenance.libraryVersion = musicanalyze::version();
        provenance.tempoMin = infrastructure::local::formatTempoBpm(used.lowBpm);
        provenance.tempoMax = infrastructure::local::formatTempoBpm(used.highBpm);
        provenance.fileSize = before->first;
        provenance.fileMtimeUnix = before->second;
        analysis->provenance = std::move(provenance);
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
