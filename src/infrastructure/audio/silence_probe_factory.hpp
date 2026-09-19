// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <string>

#include "domain/audio_content_probe.hpp"
#include "domain/matching_policy.hpp"
#include "infrastructure/local/silence_cache.hpp"

#ifdef SEABASS_HAVE_QT_AUDIO
#include "infrastructure/audio/qt_multimedia_silence_probe.hpp"
#endif

namespace seabass::infrastructure::audio
{

// The one place that decides which AudioContentProbe a caller gets, so
// the GUI's duplicate scan and anything else that grows one cannot
// drift apart -- the same mistake duration_fill.hpp exists to prevent,
// which was made once already (the duration fill was wired into the CLI
// only, and the GUI quietly found fewer duplicates as a result).
//
// Returns null when there is nothing to ask:
//
//   * the user set the wider window no higher than the exact-match one,
//     which is how "never compare audio" is expressed, or
//   * this build has no decoder AND the stick carries no cache from a
//     build that did.
//
// A null return is not a failure. DuplicateTrackFinder takes a null
// probe and falls back to comparing stored lengths alone, which is
// exactly what it did before any of this existed.
//
// `stickRoot` is the stick's mount point -- the cache lives on the
// stick, because the answers describe the stick's files rather than
// this machine, and so survive being carried to another computer.
inline std::unique_ptr<local::CachedAudioContentProbe> makeAudioContentProbe(const std::string &stickRoot)
{
    if (domain::MatchingPolicy::compareAudioSeconds() <= domain::MatchingPolicy::exactMatchSeconds()) {
        return nullptr;
    }

    std::unique_ptr<domain::AudioContentProbe> decoder;
#ifdef SEABASS_HAVE_QT_AUDIO
    constexpr bool haveDecoder = true;
    decoder = std::make_unique<QtMultimediaSilenceProbe>();
#else
    constexpr bool haveDecoder = false;
#endif

    auto cached = std::make_unique<local::CachedAudioContentProbe>(stickRoot, std::move(decoder));
    // No decoder AND nothing already measured is nothing to ask. Saying
    // so by returning null, rather than handing back a probe that
    // answers nullopt to everything, is what lets the caller report
    // "this build cannot decode audio" instead of silently comparing
    // nothing and reporting a clean scan.
    if (!haveDecoder && cached->size() == 0) {
        return nullptr;
    }
    return cached;
}

}  // namespace seabass::infrastructure::audio
