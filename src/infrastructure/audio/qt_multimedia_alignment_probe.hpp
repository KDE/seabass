// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/audio_alignment.hpp"
#include "domain/audio_alignment_probe.hpp"

namespace seabass::infrastructure::audio
{

namespace decoder_start
{
struct Attempt;
}

// Lines two copies of a song up by decoding both in full with
// QAudioDecoder (QtMultimedia's FFmpeg backend) and handing the samples
// to domain/audio_alignment, where the correlation and the rule that
// decides whether the answer can be believed live.
//
// Only the decoding is here: every file is brought to mono at
// alignment::AnalysisRate, counting frames from the first one the
// decoder hands over. The decoder's timeline is Engine's for an MP3 (see
// playback_timeline.hpp), which is why the answer is a shift on the
// decoder's timeline and the caller converts it.
//
// A file is usually in more than one pair (every copy of a group is
// measured against one), so the most recently used decoded files are
// kept, failures included, up to `cachedBytes` of samples. Of each file
// only what the windows can need is kept (alignment::KeptSamples): at
// most about five minutes of it, 26 MB at 22050 Hz mono, however long
// the file. While a file is decoded about half of it is held at once.
//
// Lives in seabass_audio_qt beside QtMultimediaSilenceProbe, for the
// same reason: seabass_core must keep linking no Qt. Needs a
// QCoreApplication (it runs a nested QEventLoop) but no GUI.
class QtMultimediaAlignmentProbe : public domain::AudioAlignmentProbe
{
public:
    // `timeoutMs` bounds one file's decode. A whole file is decoded
    // here, not a few seconds of it, so the backstop is far longer than
    // the silence probe's: two minutes is many times what a long mix
    // takes, and a damaged file that never finishes still ends.
    explicit QtMultimediaAlignmentProbe(int timeoutMs = 120000, std::size_t cachedBytes = 256u << 20);

    // Whether this build has a decoding backend that actually loaded.
    static bool decodingAvailable();

    Answer align(const std::string &absolutePathA, const std::string &absolutePathB) override;

    // What is kept of one file, mono, at alignment::AnalysisRate; null
    // when it cannot be decoded whole (refused, unreadable, timed out, a
    // stream whose rate changes partway). Public for the tests and for
    // timing.
    std::shared_ptr<const domain::alignment::KeptSamples> decoded(const std::string &absoluteFilePath);

    // The bytes of samples the decoded files kept right now hold.
    std::size_t cachedBytes() const;

private:
    std::shared_ptr<const domain::alignment::KeptSamples> decode(const std::string &absoluteFilePath) const;
    // The same through the decoder on FFmpeg's libraries, where this
    // build has it (analysis_pcm_source.hpp: ownDecoderInUse()).
    std::shared_ptr<const domain::alignment::KeptSamples> decodeThroughFfmpeg(const std::string &absoluteFilePath) const;
    // One start of the decoder; `gaveNothing` where it said nothing at
    // all before its first sample, which decode() starts again.
    std::shared_ptr<const domain::alignment::KeptSamples> decodeOnce(const std::string &absoluteFilePath,
                                                                     const decoder_start::Attempt &attempt,
                                                                     bool &gaveNothing) const;

    struct Entry
    {
        std::string path;
        std::shared_ptr<const domain::alignment::KeptSamples> samples;  // null: could not be decoded
    };
    int m_timeoutMs;
    std::size_t m_cachedBytes;
    std::list<Entry> m_cache;  // most recently used first
};

}  // namespace seabass::infrastructure::audio
