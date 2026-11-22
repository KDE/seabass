// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <cmath>

#include "infrastructure/audio/alignment_probe_factory.hpp"

namespace seabass::gui
{

// How Cues on Duplicate Copies and Clean Up say what lining the copies up
// measured (domain/duplicate_cue_shift.hpp), in one voice: the two pages
// move cues by the same rule and must not describe it two ways.

// "21 ms" under a second, "1.48 s" from there.
inline QString shiftAmountText(double ms)
{
    const double magnitude = std::abs(ms);
    if (magnitude < 1000.0) {
        return QStringLiteral("%1 ms").arg(static_cast<int>(std::lround(magnitude)));
    }
    return QStringLiteral("%1 s").arg(magnitude / 1000.0, 0, 'f', 2);
}

// "<copy> starts 1.48 s later than <other>, so cues are moved to match."
// `ms` is where a sound sits in `copy` minus where it sits in `other`.
inline QString shiftSentence(const QString &copy, const QString &other, double ms)
{
    return QStringLiteral("%1 starts %2 %3 than %4, so cues are moved to match.")
        .arg(copy, shiftAmountText(ms), ms > 0 ? QStringLiteral("later") : QStringLiteral("earlier"), other);
}

// Why copies that are not identical files could not be lined up, when
// the reason is this computer rather than the files; empty when audio can
// be decoded here.
inline QString noDecoderSentence(infrastructure::audio::AudioDecoding decoding)
{
    switch (decoding) {
    case infrastructure::audio::AudioDecoding::NotInThisBuild:
        return QStringLiteral("This build cannot decode audio, so copies that are not identical files could not be "
                              "lined up, and no cue is moved between them.");
    case infrastructure::audio::AudioDecoding::DidNotLoad:
        return QStringLiteral("The audio decoder did not load on this computer, so copies that are not identical "
                              "files could not be lined up, and no cue is moved between them.");
    case infrastructure::audio::AudioDecoding::Available:
        break;
    }
    return {};
}

}  // namespace seabass::gui
