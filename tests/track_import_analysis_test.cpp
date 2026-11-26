// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The import's analysis of one file (analyseSourceForImport), fed by
// synthetic sources in place of the decoder and with a gate of its own:
//
// 1. A whole source gives an analysis, and the gate's ticket comes back.
// 2. A cancel in the middle of the reading: OperationCancelled, and the
//    ticket comes back.
// 3. A decode error part of the way: no analysis, the ticket back.
// 4. A source that goes on past the longest a file may be: no analysis,
//    the reading stopped there, the ticket back.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "domain/pcm_source.hpp"
#include "gui/analysis_resources.hpp"
#include "gui/track_import_analysis.hpp"

using namespace seabass;
using namespace seabass::gui;

namespace
{

constexpr int Rate = 44100;

// A 220 Hz tone, `seconds` long. After `reads` reads
// it does what `then` says.
class ToneSource : public domain::PcmSource
{
public:
    enum class Then { Nothing, Cancel, Fail };
    ToneSource(double seconds, int reads, Then then, application::CancellationToken *cancel = nullptr)
        : m_total(static_cast<std::size_t>(seconds * Rate)), m_reads(reads), m_then(then),
          m_cancel(cancel)
    {
    }
    int sampleRate() const override { return Rate; }
    std::size_t read(std::span<float> out) override
    {
        ++asked;
        if (!m_error.empty()) {
            return 0;
        }
        if (asked > m_reads) {
            if (m_then == Then::Cancel) {
                m_cancel->cancel();
            } else if (m_then == Then::Fail) {
                m_error = "the stream broke off";
                return 0;
            }
        }
        const std::size_t count = std::min(out.size(), m_total - m_position);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 220.0 * static_cast<double>(m_position + i) / Rate));
        }
        m_position += count;
        samplesGiven = m_position;
        return count;
    }
    std::string error() const override { return m_error; }

    int asked = 0;
    std::size_t samplesGiven = 0;

private:
    std::size_t m_total;
    std::size_t m_position = 0;
    int m_reads;
    Then m_then;
    application::CancellationToken *m_cancel;
    std::string m_error;
};

}  // namespace

int main()
{
    AnalysisMemoryGate gate([]() { return std::optional<std::uint64_t>(); });
    const std::string noFile = "/nonexistent/seabass-track-import-analysis-test.wav";
    const auto run = [&](ToneSource *source, const application::CancellationToken &cancel, double maxSeconds) {
        return analyseSourceForImport(
            noFile,
            [source]() {
                // Handed out once; the test keeps its own pointer to it.
                struct Borrowed : domain::PcmSource
                {
                    ToneSource *source;
                    explicit Borrowed(ToneSource *s) : source(s) {}
                    int sampleRate() const override { return source->sampleRate(); }
                    std::size_t read(std::span<float> out) override { return source->read(out); }
                    std::string error() const override { return source->error(); }
                };
                return std::unique_ptr<domain::PcmSource>(std::make_unique<Borrowed>(source));
            },
            cancel, {}, gate, maxSeconds);
    };

    {
        ToneSource whole(20.0, 0, ToneSource::Then::Nothing);
        const application::CancellationToken cancel;
        const auto analysis = run(&whole, cancel, ImportAnalysisMaxSeconds);
        assert(analysis && !analysis->waveform.entries.empty());
        assert(whole.samplesGiven == static_cast<std::size_t>(20.0 * Rate));
        // No file to measure: no provenance.
        assert(!analysis->provenance);
        assert(gate.inHand() == 0);
        std::cout << "case 1 (a whole source: an analysis, the ticket back) OK\n";
    }
    {
        application::CancellationToken cancel;
        ToneSource cut(20.0, 3, ToneSource::Then::Cancel, &cancel);
        bool cancelled = false;
        try {
            run(&cut, cancel, ImportAnalysisMaxSeconds);
        } catch (const application::OperationCancelled &) {
            cancelled = true;
        }
        assert(cancelled && cut.asked >= 4 && cut.asked <= 5);
        assert(gate.inHand() == 0 && "the gate's ticket comes back after a cancel in the middle of a read");
        std::cout << "case 2 (a cancel in the middle of the reading: the ticket back) OK\n";
    }
    {
        ToneSource broken(20.0, 3, ToneSource::Then::Fail);
        const application::CancellationToken cancel;
        assert(!run(&broken, cancel, ImportAnalysisMaxSeconds) && "a file cut short by a decode error has no analysis");
        assert(broken.samplesGiven > 0);
        assert(gate.inHand() == 0);
        std::cout << "case 3 (a decode error part of the way: no analysis) OK\n";
    }
    {
        // A minute of audio stands in for one that never ends, given up
        // at ten seconds.
        ToneSource endless(60.0, 0, ToneSource::Then::Nothing);
        const application::CancellationToken cancel;
        assert(!run(&endless, cancel, 10.0) && "a file past the longest a file may be has no analysis");
        assert(endless.samplesGiven < static_cast<std::size_t>(60 * Rate) && "the reading stopped there");
        // Stopped at the first piece past ten seconds.
        assert(endless.samplesGiven > static_cast<std::size_t>(10 * Rate)
               && endless.samplesGiven < static_cast<std::size_t>(12 * Rate));
        assert(gate.inHand() == 0);
        assert(ImportAnalysisMaxSeconds == 24.0 * 3600.0);
        std::cout << "case 4 (a source past the longest a file may be: given up) OK\n";
    }
    std::cout << "track_import_analysis_test: all cases passed\n";
    return 0;
}
