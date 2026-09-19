// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "domain/audio_content_probe.hpp"

namespace seabass::infrastructure::audio
{

// Finds where a track's music starts and stops by decoding it with
// QAudioDecoder (QtMultimedia's FFmpeg backend) and looking for the
// first and last sample above a silence threshold.
//
// This is the one place in Seabass that looks at audio in order to
// decide something about a library, and it stays deliberately small.
// It measures silence; it does not fingerprint, and it cannot tell two
// different recordings of equal length apart. That is why it is only
// ever consulted about a pair that already agrees on artist and title
// and whose lengths are already close (see DuplicateTrackFinder), never
// as a matcher in its own right.
//
// Lives in seabass_audio_qt beside QtMultimediaDurationProbe, for the
// same reason: seabass_core, seabass-cli's Qt-free path and corpus_test
// must keep linking no Qt.
//
// Needs a QCoreApplication (it runs a nested QEventLoop) but no GUI.
class QtMultimediaSilenceProbe : public domain::AudioContentProbe
{
public:
    // `silenceDb` is the level below which a sample counts as silence,
    // in dBFS. -60 dB is well under the noise floor of a lossy encode
    // (which is never digitally silent even where the source was) and
    // well under any music -- a fade's last audible moment sits far
    // above it.
    //
    // `timeoutMs` bounds one file. A damaged file can otherwise leave
    // the backend never emitting Finished, which would hang a scan on
    // one bad track. It is generous compared to the duration probe's
    // because this actually decodes: measured on this machine, a
    // six-minute 320 kbps mp3 decodes in roughly 0.4 s.
    explicit QtMultimediaSilenceProbe(double silenceDb = -60.0, int timeoutMs = 30000);

    std::optional<domain::AudioContentSpan> measure(const std::string &absoluteFilePath) override;

private:
    double m_silenceThreshold;  // linear amplitude, 0..1
    int m_timeoutMs;
};

}  // namespace seabass::infrastructure::audio
