// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

namespace musicanalyze
{

// One beat of a track's beat grid.
struct Beat
{
    // From the first sample of the audio.
    double timeMs = 0.0;
    // 1 to 4, 1 being the downbeat; 0 where the source does not say.
    int beatInBar = 0;
};

}  // namespace musicanalyze
