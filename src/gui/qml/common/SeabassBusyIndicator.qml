// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The busy spinner every page uses: a BusyIndicator, so `running`, sizes
// and visibility work as before, drawn here rather than by the style. The
// style's own was a ring of dark grey dots, and on the dark surfaces it
// sits on -- a stick's row while it ejects, above all -- it was all but
// invisible. This is an accent arc turning over a faint full ring, the
// ring saying where the spinner is even in the frame the arc is short.
BusyIndicator {
    id: control
    implicitWidth: Theme.scaled(24)
    implicitHeight: Theme.scaled(24)
    padding: 0

    contentItem: Item {
        implicitWidth: control.implicitWidth
        implicitHeight: control.implicitHeight
        opacity: control.running ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Canvas {
            id: arc
            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height)
            height: width
            readonly property real stroke: Math.max(2, width / 9)
            onWidthChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                const r = (width - stroke) / 2;
                ctx.lineWidth = stroke;
                ctx.lineCap = "round";
                ctx.strokeStyle = Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.18);
                ctx.beginPath();
                ctx.arc(width / 2, height / 2, r, 0, 2 * Math.PI);
                ctx.stroke();
                ctx.strokeStyle = Theme.accent;
                ctx.beginPath();
                ctx.arc(width / 2, height / 2, r, -Math.PI / 2, Math.PI * 0.9);
                ctx.stroke();
            }
            RotationAnimator on rotation {
                running: control.running && control.visible
                from: 0
                to: 360
                duration: 900
                loops: Animation.Infinite
            }
        }
    }
}
