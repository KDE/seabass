// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Layouts
import SeabassGui

// Holds one header button and makes its entrance: it drops in from
// above, spinning, swells past its size and springs back. play() runs
// it, `delay` staggers a row of them into a cascade. vanish() is the way
// out: back up where it came from, fading.
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
    // True from vanish() until the button is gone; whoever hides the slot
    // keeps it shown until then, or there is nothing to watch go.
    readonly property bool vanishing: exit.running

    implicitWidth: slot.item ? slot.item.implicitWidth : 0
    implicitHeight: slot.item ? slot.item.implicitHeight : 0
    Layout.preferredWidth: implicitWidth
    Layout.preferredHeight: implicitHeight

    function play() {
        entrance.stop();
        exit.stop();
        holder.opacity = 0;
        drop.y = -Theme.scaled(28);
        spin.angle = -270;
        swell.xScale = 0.2;
        swell.yScale = 0.2;
        entrance.start();
    }

    // Back in place at once, no entrance: for a page that shows the
    // header without coming from home, after the slot had vanished.
    function show() {
        entrance.stop();
        exit.stop();
        holder.opacity = 1;
        drop.y = 0;
        spin.angle = 0;
        swell.xScale = 1;
        swell.yScale = 1;
    }

    // `after` milliseconds from now, so a row can leave in a cascade too.
    function vanish(after) {
        entrance.stop();
        exitPause.duration = after || 0;
        exit.start();
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
    SequentialAnimation {
        id: exit
        PauseAnimation { id: exitPause; duration: 0 }
        ParallelAnimation {
            // Back up where it came from: up by the height it dropped,
            // fading as it goes, speeding away rather than settling.
            NumberAnimation { target: drop; property: "y"; to: -Theme.scaled(28); duration: 240; easing.type: Easing.InQuad }
            NumberAnimation { target: holder; property: "opacity"; to: 0; duration: 240; easing.type: Easing.InQuad }
        }
    }
}
