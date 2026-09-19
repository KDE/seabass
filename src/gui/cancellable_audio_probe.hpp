// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>
#include <utility>

#include "application/ports/cancellation_token.hpp"
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

}  // namespace seabass::gui
