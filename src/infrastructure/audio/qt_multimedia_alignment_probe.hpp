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

#include "domain/audio_alignment_probe.hpp"

namespace seabass::infrastructure::audio
{

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
// measured against the first), so the last few decoded files are kept,
// failures included. A seven-minute file is 37 MB at 22050 Hz mono, so
// only a handful: `cachedFiles` of them.
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
    explicit QtMultimediaAlignmentProbe(int timeoutMs = 120000, std::size_t cachedFiles = 3);

    // Whether this build has a decoding backend that actually loaded.
    static bool decodingAvailable();

    Answer align(const std::string &absolutePathA, const std::string &absolutePathB) override;

    // One file, mono, at alignment::AnalysisRate; nullopt when it cannot
    // be decoded whole (refused, unreadable, timed out, a stream whose
    // rate changes partway). Public for the tests and for timing.
    std::shared_ptr<const std::vector<float>> decoded(const std::string &absoluteFilePath);

private:
    std::shared_ptr<const std::vector<float>> decode(const std::string &absoluteFilePath) const;

    struct Entry
    {
        std::string path;
        std::shared_ptr<const std::vector<float>> samples;  // null: could not be decoded
    };
    int m_timeoutMs;
    std::size_t m_cachedFiles;
    std::list<Entry> m_cache;  // most recently used first
};

}  // namespace seabass::infrastructure::audio
