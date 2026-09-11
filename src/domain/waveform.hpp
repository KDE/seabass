// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

namespace seabass::domain
{

// One column of a waveform preview, split into three frequency bands (each
// normalized to 0..1) so the GUI can render a colored waveform the way DJ
// hardware does -- low/bass in one hue, mid in another, high in a third.
// Not every source can populate all three distinctly: see each reader's own
// comment for what its underlying format actually provides.
struct WaveformColumn
{
    double low = 0.0;
    double mid = 0.0;
    double high = 0.0;
};

}  // namespace seabass::domain
