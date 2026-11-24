// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A confirmation that something was done, said over the thing it was done
// to and gone again: Sync Cue Points' checkmark and "N cues synced", for
// a page that stays as it is. show(text) shows it for `duration`, a thin
// ring around the mark unwinding meanwhile so the eye knows it will go,
// then it lifts and dissolves; a click sends it away at once, and the
// pointer resting on it holds it (the ring stops with it). show() again
// restarts it with the new text. Place it where the eye is (centred over
// the list that changed); it takes no space when it is not showing.
Item {
    id: toast
    // Six seconds: three read as gone before it was read (Sebastian,
    // 2026-10-31), and the pointer resting on it holds it anyway.
    property int duration: 6000
    property string text: ""
    readonly property bool showing: internal.shown
    signal dismissed()

    // Held: the pointer is over it, and the countdown waits.
    readonly property bool held: hover.hovered

    function show(message) {
        toast.text = message;
        internal.shown = true;
        countdown.restart();
        if (toast.held) {
            countdown.pause();
        }
    }

    function dismiss() {
        if (!internal.shown) {
            return;
        }
        countdown.stop();
        internal.shown = false;
        toast.dismissed();
    }

    QtObject {
        id: internal
        property bool shown: false
    }

    onHeldChanged: {
        if (!countdown.running) {
            return;
        }
        if (toast.held) {
            countdown.pause();
        } else {
            countdown.resume();
        }
    }

    HoverHandler {
        id: hover
    }

    implicitWidth: panel.implicitWidth
    implicitHeight: panel.implicitHeight
    visible: opacity > 0
    // How far it has floated up: nothing while shown, a little as it goes.
    property real lift: 0

    // Never over a progress bar (#122). A save's toast goes up in the same
    // call that starts reading the stick again, and every page centres
    // both: the toast sat on the bar of the very refresh it started. The
    // page still places it; if that place covers an operation's progress
    // (ProgressAreas), it steps to the nearest place above or below that
    // covers none and stays on its parent. Up first, on a tie.
    readonly property real clearance: {
        const host = toast.parent;
        const areas = ProgressAreas.items;
        if (!host || areas.length === 0) {
            return 0;
        }
        const top = toast.y;
        const left = toast.x;
        const right = toast.x + toast.width;
        const h = toast.height;
        const room = host.height;
        const gap = Theme.sectionSpacing;
        const ownWindow = toast.Window.window;
        const blocks = [];
        for (const area of areas) {
            // x and y read here so the binding follows the area as it moves.
            if (!area.visible || area.width <= 0 || area.height <= 0 || !isFinite(area.x + area.y)
                    || area.Window.window !== ownWindow) {
                continue;
            }
            const r = area.mapToItem(host, 0, 0, area.width, area.height);
            if (r.x < right && left < r.x + r.width) {
                blocks.push({top: r.y, bottom: r.y + r.height});
            }
        }
        const free = (y) => blocks.every((b) => y + h <= b.top || y >= b.bottom);
        if (free(top)) {
            return 0;
        }
        let best = null;
        for (const b of blocks) {
            for (const y of [b.top - gap - h, b.bottom + gap]) {
                if (y < 0 || y + h > room || !free(y)) {
                    continue;
                }
                if (best === null || Math.abs(y - top) < Math.abs(best - top)
                        || (Math.abs(y - top) === Math.abs(best - top) && y < best)) {
                    best = y;
                }
            }
        }
        return best === null ? 0 : best - top;
    }

    transform: [
        Translate { y: -toast.lift },
        Translate {
            y: toast.clearance
            // A glide, not a jump, when a bar comes up under a toast that
            // is on screen, fading out included; none while it is hidden.
            Behavior on y {
                enabled: toast.visible
                NumberAnimation { duration: 2 * Theme.shortTransitionDuration; easing.type: Easing.OutCubic }
            }
        }
    ]
    // A long text wraps inside the page rather than running off both
    // sides of a narrow one. The floor keeps a few words to a line; on a
    // page narrower than even that, the toast itself is held to the
    // page's width below.
    readonly property real textRoom: toast.parent
        ? Math.max(Theme.scaled(40), toast.parent.width - 2 * Theme.pageMargin - markBox.width
                   - Theme.rowSpacing - panel.leftPadding - panel.rightPadding)
        : Number.POSITIVE_INFINITY
    width: toast.parent ? Math.min(implicitWidth, toast.parent.width) : implicitWidth

    // In: a quick pop with a touch of overshoot. Out: it lifts and
    // dissolves, slower, so the eye sees it leave rather than blink off.
    opacity: 0
    scale: 0.9
    states: State {
        name: "shown"
        when: internal.shown
        PropertyChanges { toast.opacity: 1; toast.scale: 1; toast.lift: 0 }
    }
    transitions: [
        Transition {
            to: "shown"
            NumberAnimation {
                properties: "opacity,scale,lift"
                duration: 2 * Theme.shortTransitionDuration
                easing.type: Easing.OutBack
            }
        },
        Transition {
            from: "shown"
            ParallelAnimation {
                NumberAnimation { property: "opacity"; duration: 4 * Theme.shortTransitionDuration; easing.type: Easing.InCubic }
                NumberAnimation { property: "scale"; to: 1.04; duration: 4 * Theme.shortTransitionDuration; easing.type: Easing.InCubic }
                NumberAnimation { property: "lift"; to: Theme.scaled(14); duration: 4 * Theme.shortTransitionDuration; easing.type: Easing.InCubic }
            }
        }
    ]

    Pane {
        id: panel
        objectName: "confirmationToastPanel"
        width: toast.width
        padding: Theme.rowSpacing
        topPadding: Theme.scaled(14)
        bottomPadding: Theme.scaled(14)
        background: Rectangle {
            color: Theme.surface
            border.color: Theme.borderSubtle
            radius: Theme.cornerRadius
        }
        // On the Pane, not the toast: a Control takes the press itself, so
        // a handler behind it would never hear of the click.
        TapHandler {
            enabled: internal.shown
            onTapped: toast.dismiss()
        }
        contentItem: Row {
            spacing: Theme.rowSpacing
            Item {
                id: markBox
                width: Theme.scaled(52)
                height: width
                anchors.verticalCenter: parent.verticalCenter
                // 1 at show(), 0 when the toast goes: the ring's remaining
                // arc, and the toast's clock: the ring run down is the end.
                property real remaining: 1
                NumberAnimation {
                    id: countdown
                    target: markBox
                    property: "remaining"
                    from: 1
                    to: 0
                    duration: toast.duration
                }
                // The ring run down is the end. Not the animation's
                // finished(): a restart stops it first, and that must
                // not count as its time being up.
                onRemainingChanged: {
                    if (remaining <= 0 && internal.shown) {
                        toast.dismiss();
                    }
                }
                CheckmarkCanvas {
                    objectName: "confirmationToastMark"
                    anchors.fill: parent
                    remaining: markBox.remaining
                }
            }
            Label {
                objectName: "confirmationToastLabel"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, toast.textRoom)
                wrapMode: Text.Wrap
                text: toast.text
                color: Theme.good
                font.pointSize: Theme.fontLarge
            }
        }
    }

}
