// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "analysis_baseline/baseline_analysers.hpp"

#include <array>
#include <cmath>

#include "analysis_baseline/mixxx_beatutils.hpp"
#include "analysis_baseline/mixxx_qm_plugins.hpp"

namespace seabass::baseline
{

namespace
{

std::string sourceFailure(const domain::PcmSource &source)
{
    return source.error().empty() ? std::string() : source.error();
}

double frameToMs(double frame, int sampleRate)
{
    return frame * 1000.0 / sampleRate;
}

class QmRawAnalyser : public domain::GridAnalyser
{
public:
    std::string name() const override { return "qm-raw"; }
    domain::GridAnalysis analyse(domain::PcmSource &source) override
    {
        domain::GridAnalysis analysis;
        const QmBeats tracked = trackQueenMaryBeats(source);
        if (!tracked.error.empty()) {
            analysis.error = tracked.error;
            return analysis;
        }
        analysis.beats.reserve(tracked.beatFrames.size());
        for (const double frame : tracked.beatFrames) {
            analysis.beats.push_back({frameToMs(frame, tracked.sampleRate), 0});
        }
        return analysis;
    }
};

class MixxxConstantAnalyser : public domain::GridAnalyser
{
public:
    explicit MixxxConstantAnalyser(bool roundBpm) : m_roundBpm(roundBpm) {}
    std::string name() const override { return m_roundBpm ? "mixxx-constant" : "mixxx-constant-unrounded"; }
    domain::GridAnalysis analyse(domain::PcmSource &source) override
    {
        domain::GridAnalysis analysis;
        const QmBeats tracked = trackQueenMaryBeats(source);
        if (!tracked.error.empty()) {
            analysis.error = tracked.error;
            return analysis;
        }
        // An empty grid where Mixxx makes none: the track has no tempo
        // for this method, which the benchmark reports as no beats.
        analysis.beats =
            mixxxConstantGrid(tracked.beatFrames, tracked.sampleRate, tracked.totalFrames, m_roundBpm);
        return analysis;
    }

private:
    bool m_roundBpm;
};

class QmKeyAnalyser : public KeyAnalyser
{
public:
    std::string name() const override { return "qm-key"; }
    KeyAnalysis analyse(domain::PcmSource &source) override
    {
        KeyAnalysis analysis;
        const int rate = source.sampleRate();
        if (rate <= 0) {
            analysis.error = source.error().empty() ? "no sample rate" : source.error();
            return analysis;
        }
        mixxx::QueenMaryKey detector;
        if (!detector.initialize(rate)) {
            analysis.error = "the key detector would not start";
            return analysis;
        }
        std::vector<float> chunk(mixxx::AnalysisFramesPerChunk);
        double totalFrames = 0.0;
        bool running = true;
        for (;;) {
            const std::size_t got = source.read(chunk);
            if (got == 0) {
                break;
            }
            totalFrames += static_cast<double>(got);
            // Mixxx stops feeding a plugin that said no, and goes on
            // reading; here the rest of the source is drained the same way.
            if (running) {
                running = detector.processSamples(chunk.data(), got);
            }
        }
        if (const std::string failure = sourceFailure(source); !failure.empty()) {
            analysis.error = failure;
            return analysis;
        }
        if (!detector.finalize()) {
            analysis.error = "the key detector gave a key out of range";
            return analysis;
        }
        analysis.key = chromaticKeyName(mixxx::calculateGlobalKey(detector.keyChanges(), totalFrames));
        return analysis;
    }
};

}  // namespace

QmBeats trackQueenMaryBeats(domain::PcmSource &source)
{
    QmBeats result;
    result.sampleRate = source.sampleRate();
    if (result.sampleRate <= 0) {
        result.error = source.error().empty() ? "no sample rate" : source.error();
        return result;
    }
    mixxx::QueenMaryBeats tracker;
    if (!tracker.initialize(result.sampleRate)) {
        result.error = "the beat tracker would not start";
        return result;
    }
    std::vector<float> chunk(mixxx::AnalysisFramesPerChunk);
    for (;;) {
        const std::size_t got = source.read(chunk);
        if (got == 0) {
            break;
        }
        result.totalFrames += static_cast<double>(got);
        tracker.processSamples(chunk.data(), got);
    }
    if (const std::string failure = sourceFailure(source); !failure.empty()) {
        result.error = failure;
        return result;
    }
    if (result.totalFrames == 0.0) {
        result.error = "no samples";
        return result;
    }
    tracker.finalize();
    result.beatFrames = tracker.beats();
    return result;
}

std::vector<domain::Beat> mixxxConstantGrid(const std::vector<double> &beatFrames, int sampleRate,
                                            double totalFrames, bool roundBpm, double *bpm)
{
    std::vector<domain::Beat> beats;
    const auto grid = mixxx::makeConstGrid(beatFrames, sampleRate, roundBpm);
    if (bpm) {
        *bpm = grid ? grid->bpm : 0.0;
    }
    if (!grid) {
        return beats;
    }
    // A constant grid has a beat every beat length either side of its
    // anchor; the ones inside the track are its beats. Each is placed
    // from the anchor by multiplication, so no error adds up.
    const double beatLength = 60.0 * sampleRate / grid->bpm;
    const auto firstIndex = static_cast<long>(std::ceil(-grid->firstBeatFrame / beatLength));
    for (long k = firstIndex;; ++k) {
        const double frame = grid->firstBeatFrame + static_cast<double>(k) * beatLength;
        if (frame >= totalFrames) {
            break;
        }
        beats.push_back({frameToMs(frame, sampleRate), 0});
    }
    return beats;
}

std::vector<std::string> baselineGridAnalyserNames()
{
    return {"qm-raw", "mixxx-constant", "mixxx-constant-unrounded"};
}

std::unique_ptr<domain::GridAnalyser> makeBaselineGridAnalyser(const std::string &name)
{
    if (name == "qm-raw") {
        return std::make_unique<QmRawAnalyser>();
    }
    if (name == "mixxx-constant") {
        return std::make_unique<MixxxConstantAnalyser>(true);
    }
    if (name == "mixxx-constant-unrounded") {
        return std::make_unique<MixxxConstantAnalyser>(false);
    }
    return nullptr;
}

std::vector<std::string> baselineKeyAnalyserNames()
{
    return {"qm-key"};
}

std::unique_ptr<KeyAnalyser> makeBaselineKeyAnalyser(const std::string &name)
{
    if (name == "qm-key") {
        return std::make_unique<QmKeyAnalyser>();
    }
    return nullptr;
}

std::string chromaticKeyName(int key)
{
    static const std::array<const char *, 12> Tonics = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    if (key >= 1 && key <= 12) {
        return Tonics[static_cast<std::size_t>(key - 1)];
    }
    if (key >= 13 && key <= 24) {
        return std::string(Tonics[static_cast<std::size_t>(key - 13)]) + "m";
    }
    return {};
}

}  // namespace seabass::baseline
