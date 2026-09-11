// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A stat/count/measurement value (track counts, BPM, durations) --
// tabular mono, never bold, so columns of numbers actually line up.
// See docs/design/type-scale.html.
Label {
    // Monospace family already gives every digit the same width, so
    // columns of these line up without needing a tabular-figures
    // OpenType feature.
    font.family: Theme.dataFamily
    font.weight: Font.Medium
    font.pointSize: Theme.dataSize
}
