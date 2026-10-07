// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Layouts
import SeabassGui

// Holds one header button and makes its entrance: it drops in from
// above, spinning, swells past its size and springs back. play() runs
// it, `delay` staggers a row of them into a cascade.
//
// The movement is on transforms of a holder around the button, never on
// the button's own scale or rotation: the Support heart beats on its own
// scale, and the two would fight.
Item {
    id: slot

    default property alias content: holder.data
    readonly property Item item: holder.children.length > 0 ? holder.children[0] : null
    // Milliseconds after play() before this one moves.
    property int delay: 0
    readonly property bool running: entrance.running

    implicitWidth: slot.item ? slot.item.implicitWidth : 0
    implicitHeight: slot.item ? slot.item.implicitHeight : 0
    Layout.preferredWidth: implicitWidth
    Layout.preferredHeight: implicitHeight

    function play() {
        entrance.stop();
        holder.opacity = 0;
        drop.y = -Theme.scaled(28);
        spin.angle = -270;
        swell.xScale = 0.2;
        swell.yScale = 0.2;
        entrance.start();
    }

    Item {
        id: holder
        width: slot.width
        height: slot.height
        transform: [
            Scale {
                id: swell
                origin.x: holder.width / 2
                origin.y: holder.height / 2
            },
            Rotation {
                id: spin
                origin.x: holder.width / 2
                origin.y: holder.height / 2
            },
            Translate { id: drop }
        ]
    }

    SequentialAnimation {
        id: entrance
        PauseAnimation { duration: slot.delay }
        ParallelAnimation {
            NumberAnimation { target: holder; property: "opacity"; to: 1; duration: 140; easing.type: Easing.OutQuad }
            NumberAnimation { target: drop; property: "y"; to: 0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 2.2 }
            NumberAnimation { target: spin; property: "angle"; to: 0; duration: 620; easing.type: Easing.OutBack; easing.overshoot: 1.4 }
            SequentialAnimation {
                NumberAnimation { targets: [swell]; properties: "xScale,yScale"; to: 1.35; duration: 300; easing.type: Easing.OutQuad }
                NumberAnimation { targets: [swell]; properties: "xScale,yScale"; to: 1.0; duration: 700; easing.type: Easing.OutElastic; easing.amplitude: 1.2; easing.period: 0.35 }
            }
        }
    }
}
