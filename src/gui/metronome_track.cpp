// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "metronome_track.hpp"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <span>

#include "analysis_settings.hpp"
#include "async_request.hpp"
#include "domain/beat_timeline.hpp"
#include "future_result.hpp"
#include "infrastructure/audio/analysis_pcm_source.hpp"
#include "musicanalyze/analysis.hpp"
#include "seabass_settings.hpp"

namespace seabass::gui
{

MetronomeTrack::MetronomeTrack(QObject *parent)
    : QObject(parent)
{
}

MetronomeTrack::~MetronomeTrack()
{
    // Told to stop and not waited for: the analysis holds its own
    // copies and touches nothing of this object, so it ends alone, at
    // its next piece of audio. The end of the process sees to it like
    // to every other read (endProcess()).
    if (m_stop) {
        m_stop->store(true);
    }
}

double MetronomeTrack::lengthMs() const
{
    return static_cast<double>(m_levels.size()) * 1000.0 / musicanalyze::WaveformEntriesPerSecond;
}

void MetronomeTrack::load(const QUrl &file)
{
    if (!file.isLocalFile()) {
        return;
    }
    if (m_stop) {
        m_stop->store(true);
    }
    m_stop = std::make_shared<std::atomic<bool>>(false);
    m_busy = true;
    m_error.clear();
    m_title = QFileInfo(file.toLocalFile()).completeBaseName();
    m_summary.clear();
    m_timing.clear();
    m_beatsMs.clear();
    m_levels.clear();
    m_source.clear();
    emit changed();
    // The older analysis, if one runs, has been told to stop; its
    // watcher goes, and with it its answer.
    if (m_watcher) {
        m_watcher->disconnect(this);
        m_watcher->deleteLater();
    }
    const QSettings settings = openSeabassSettings();
    const domain::TempoRange range = storedAnalysisTempoRange(settings);
    auto *watcher = new QFutureWatcher<Result>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, file] {
        watcher->deleteLater();
        if (watcher != m_watcher) {
            return;
        }
        // What the analysis threw, if it threw, is this file's error.
        QString thrown;
        Result result = takeResult(*watcher, &thrown);
        if (!thrown.isEmpty()) {
            result = Result{};
            result.error = thrown;
        }
        take(file, result);
    });
    // A read, so on a thread of its own and never on the global pool,
    // which the end of the process waits for without limit.
    const QString path = file.toLocalFile();
    const std::shared_ptr<std::atomic<bool>> stop = m_stop;
    watcher->setFuture(runRead([path, range, stop] { return analyseFile(path, range.lowBpm, range.highBpm, stop); }));
}

MetronomeTrack::Result MetronomeTrack::analyseFile(const QString &path, double lowBpm, double highBpm,
                                                   const std::shared_ptr<std::atomic<bool>> &stop)
{
    Result result;
    QElapsedTimer whole;
    whole.start();
    if (stop->load()) {
        result.error = QStringLiteral("Stopped");
        return result;
    }
    if (!infrastructure::audio::analysisDecodingAvailable()) {
        result.error = QStringLiteral("This computer has no audio decoder Seabass can use");
        return result;
    }
    const std::unique_ptr<domain::PcmSource> opened = infrastructure::audio::openAnalysisPcmSource(path.toStdString());
    domain::PcmSource &decoder = *opened;
    if (decoder.sampleRate() <= 0) {
        result.error = QStringLiteral("This file could not be decoded");
        return result;
    }
    musicanalyze::Analyzer analyzer(decoder.sampleRate(), musicanalyze::TempoRange{lowBpm, highBpm});
    std::vector<float> piece(65536);
    QElapsedTimer inAnalyzer;
    qint64 analyzerNs = 0;
    bool fedAny = false;
    for (;;) {
        if (stop->load()) {
            result.error = QStringLiteral("Stopped");
            return result;
        }
        const std::size_t got = decoder.read(piece);
        if (got == 0) {
            break;
        }
        inAnalyzer.start();
        analyzer.feed(std::span<const float>(piece.data(), got));
        analyzerNs += inAnalyzer.nsecsElapsed();
        fedAny = true;
    }
    // A decoder that stopped on an error has not given the whole file,
    // and a grid of part of a track would look like the track's.
    if (!decoder.error().empty()) {
        result.error = QStringLiteral("This file could not be decoded to its end");
        return result;
    }
    if (!fedAny) {
        result.error = QStringLiteral("This file could not be decoded");
        return result;
    }
    inAnalyzer.start();
    const musicanalyze::Analysis found = analyzer.finish();
    analyzerNs += inAnalyzer.nsecsElapsed();
    result.analysisMs = static_cast<double>(analyzerNs) / 1e6;
    result.wholeMs = static_cast<double>(whole.nsecsElapsed()) / 1e6;
    if (!found.grid.error.empty()) {
        result.error = QString::fromStdString(found.grid.error);
        return result;
    }
    result.beatsMs.reserve(found.grid.beats.size());
    for (const musicanalyze::Beat &beat : found.grid.beats) {
        result.beatsMs.push_back(beat.timeMs);
    }
    result.levels.reserve(found.waveform.entries.size());
    for (const musicanalyze::WaveformEntry &entry : found.waveform.entries) {
        result.levels.push_back(entry.overall);
    }
    result.bpm = found.bpm;
    if (found.key.key) {
        result.key = QString::fromStdString(musicanalyze::keyName(*found.key.key));
    }
    result.followsTempo = found.grid.followsTempo;
    return result;
}

void MetronomeTrack::take(const QUrl &file, const Result &result)
{
    m_busy = false;
    m_beatsMs.clear();
    m_levels.clear();
    m_source.clear();
    m_summary.clear();
    m_timing.clear();
    m_error.clear();
    if (m_title.isEmpty()) {
        m_title = QFileInfo(file.toLocalFile()).completeBaseName();
    }
    if (!result.error.isEmpty()) {
        m_error = result.error;
    } else if (result.beatsMs.size() < 2) {
        m_error = QStringLiteral("No beat found in this one");
    } else {
        m_beatsMs = result.beatsMs;
        m_levels = result.levels;
        QStringList parts{QStringLiteral("%1 BPM").arg(result.bpm, 0, 'f', 2)};
        if (!result.key.isEmpty()) {
            parts << result.key;
        }
        parts << (result.followsTempo ? QStringLiteral("tempo moves") : QStringLiteral("steady tempo"));
        parts << QStringLiteral("%1 beats").arg(m_beatsMs.size());
        m_summary = parts.join(QStringLiteral("  \u00b7  "));
        m_timing = QStringLiteral("Analysed in %1 s, %2 s with decoding")
                       .arg(result.analysisMs / 1000.0, 0, 'f', 2)
                       .arg(result.wholeMs / 1000.0, 0, 'f', 2);
        m_source = file;
    }
    emit changed();
    if (!m_beatsMs.empty()) {
        emit ready();
    }
}

int MetronomeTrack::beatAt(double ms) const
{
    return domain::beatAt(m_beatsMs, ms);
}

double MetronomeTrack::phaseAt(double ms) const
{
    return domain::beatPhaseAt(m_beatsMs, ms);
}

QVariantList MetronomeTrack::beatsBetween(double fromMs, double toMs) const
{
    QVariantList out;
    auto beat = std::lower_bound(m_beatsMs.begin(), m_beatsMs.end(), fromMs);
    for (; beat != m_beatsMs.end() && *beat <= toMs; ++beat) {
        out << *beat << static_cast<int>(beat - m_beatsMs.begin());
    }
    return out;
}

QVariantList MetronomeTrack::levels(double fromMs, double toMs, int columns) const
{
    QVariantList out;
    // As many columns as a wide screen has pixels and no more: the
    // number comes from QML.
    columns = std::min(columns, 16384);
    for (const float level : domain::loudestPerColumn(m_levels, musicanalyze::WaveformEntriesPerSecond, fromMs, toMs, columns)) {
        out << level;
    }
    return out;
}

}  // namespace seabass::gui
