// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A card: one of a set of peers, or a block that stands apart from the
// page -- a transfer's progress, a track's waveform. Rounded, in the
// surface colour, with an edge, drawn here rather than by the style, so
// it is the same card on Linux, macOS and Windows. A plain Frame was a
// bordered see-through box under KDE's style and a filled one elsewhere.
// For a page's own sections, see PageSection.
Frame {
    padding: Theme.cardPadding
    leftPadding: Theme.cardPadding
    rightPadding: Theme.cardPadding
    topPadding: Theme.cardPadding
    bottomPadding: Theme.cardPadding
    background: Rectangle {
        radius: Theme.cornerRadius
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
    }
}
