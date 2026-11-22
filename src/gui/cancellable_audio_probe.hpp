// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>
#include <utility>

#include "application/ports/cancellation_token.hpp"
#include "domain/audio_alignment_probe.hpp"
#include "domain/audio_content_probe.hpp"

namespace seabass::gui
{

// Makes the decoding half of a duplicate scan answer to Cancel.
//
// DuplicateTrackFinder is domain and takes no cancellation token, which
// is fine when it only compares numbers. Once it may decode audio it is
// the slowest thing in the scan, and the "Finding duplicates..." phase
// was uncancellable for as long as the decoding took. Rather than
// thread a token through the domain signature for one caller, the probe
// itself stops answering: a cancelled scan then costs at most one file
// in flight, the finder walks out of its loops comparing nothing, and
// the controller discards the result as it already does.
//
// Answering nullopt rather than throwing on purpose. Every caller of a
// probe already treats "no answer" as "no opinion, not a match", so a
// cancelled scan degrades to exactly the grouping stored lengths alone
// would have produced -- never to a wrong one.
class CancellableAudioContentProbe : public domain::AudioContentProbe
{
public:
    // Borrows `inner`, which must outlive this and may be null (the
    // "nothing to ask" case every caller already handles). Non-owning
    // because the caller holds the real probe in order to save its
    // cache and read its counters after the scan.
    CancellableAudioContentProbe(domain::AudioContentProbe *inner, application::CancellationToken cancel)
        : m_inner(inner), m_cancel(std::move(cancel))
    {
    }

    std::optional<domain::AudioContentSpan> measure(const std::string &absoluteFilePath) override
    {
        if (m_inner == nullptr || m_cancel.cancelled()) {
            return std::nullopt;
        }
        return m_inner->measure(absoluteFilePath);
    }

private:
    domain::AudioContentProbe *m_inner;
    application::CancellationToken m_cancel;
};

// The same for lining copies up (domain/audio_alignment_probe.hpp), which
// decodes every copy of every group with cues in full and is by far the
// longest part of a scan that has any. A cancelled scan stops after the
// file in flight; nullopt is "unknown", which never moves a cue, and the
// scan then ends as cancelled (application::measureDuplicateCueShifts).
class CancellableAudioAlignmentProbe : public domain::AudioAlignmentProbe
{
public:
    // Borrows `inner`, which must outlive this and may be null.
    CancellableAudioAlignmentProbe(domain::AudioAlignmentProbe *inner, application::CancellationToken cancel)
        : m_inner(inner), m_cancel(std::move(cancel))
    {
    }

    std::optional<double> shiftMs(const std::string &absolutePathA, const std::string &absolutePathB) override
    {
        if (m_inner == nullptr || m_cancel.cancelled()) {
            return std::nullopt;
        }
        return m_inner->shiftMs(absolutePathA, absolutePathB);
    }

    bool sameContent(const std::string &absolutePathA, const std::string &absolutePathB) override
    {
        if (m_inner == nullptr || m_cancel.cancelled()) {
            return false;
        }
        return m_inner->sameContent(absolutePathA, absolutePathB);
    }

private:
    domain::AudioAlignmentProbe *m_inner;
    application::CancellationToken m_cancel;
};

}  // namespace seabass::gui
