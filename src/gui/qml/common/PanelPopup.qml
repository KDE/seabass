// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A panel that floats over the page -- the Camelot wheel, a merge
// review, a track's details: a rounded card a step above the page, with
// an edge, on every platform. A plain Popup takes its background from
// the style, which made the same popup a square borderless box in one
// place and a bordered one in another.
Popup {
    // Every side named, as in Card: a style setting leftPadding and the
    // like would otherwise win over `padding`.
    padding: Theme.cardPadding
    leftPadding: Theme.cardPadding
    rightPadding: Theme.cardPadding
    topPadding: Theme.cardPadding
    bottomPadding: Theme.cardPadding
    background: Rectangle {
        radius: Theme.popupRadius
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
    }
}
