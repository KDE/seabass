// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "domain/audio_alignment_probe.hpp"

namespace seabass::infrastructure::audio
{

// True only when both files can be read and hold the same bytes: equal
// sizes on disk, then every byte compared. Not the catalogs' recorded
// sizes, which two different encodes can share and which two of the three
// formats do not record at all. A file that cannot be read is never
// identical to anything.
bool filesHaveSameBytes(const std::string &absolutePathA, const std::string &absolutePathB);

// The AudioAlignmentProbe every caller is handed: two copies that are the
// same file byte for byte are lined up by definition (shift 0) and are
// never decoded; anything else goes to `decoder`. Without a decoder (a
// build with no Qt Multimedia) everything that is not byte-identical is
// unknown, never 0.
//
// Re-exports are byte-identical copies far more often than not, so this
// is also what keeps most pairs from costing two full decodes.
class IdenticalFileAlignmentProbe : public domain::AudioAlignmentProbe
{
public:
    explicit IdenticalFileAlignmentProbe(std::unique_ptr<domain::AudioAlignmentProbe> decoder)
        : m_decoder(std::move(decoder))
    {
    }

    Answer align(const std::string &absolutePathA, const std::string &absolutePathB) override;
    // filesHaveSameBytes(), remembered for the last pair asked, so asking
    // this and then align() about one pair reads the files once.
    bool sameContent(const std::string &absolutePathA, const std::string &absolutePathB) override;

    bool hasDecoder() const { return m_decoder != nullptr; }
    // What this probe has been asked: pairs found byte-identical, pairs
    // the decoder lined up, and pairs left unknown (not lined up,
    // unreadable, or no decoder to ask).
    int identicalCount() const { return m_identical; }
    int measuredCount() const { return m_measured; }
    int unknownCount() const { return m_unknown; }

private:
    std::unique_ptr<domain::AudioAlignmentProbe> m_decoder;
    std::string m_lastA;
    std::string m_lastB;
    bool m_lastSame = false;
    int m_identical = 0;
    int m_measured = 0;
    int m_unknown = 0;
};

}  // namespace seabass::infrastructure::audio
