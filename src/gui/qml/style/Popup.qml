// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Templates as T
import SeabassGui

// A popup with an edge. Basic's is a square, borderless box in the
// window colour, so a popup laid over a card of the same colour had no
// outline at all and no corner to match the cards beneath it.
T.Popup {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: Theme.cardPadding

    background: Rectangle {
        radius: Theme.popupRadius
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
    }

    T.Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.3) }
    T.Overlay.modeless: Rectangle { color: Qt.rgba(0, 0, 0, 0.15) }
}
