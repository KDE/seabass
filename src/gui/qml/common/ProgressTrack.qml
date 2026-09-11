// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The progress bar itself, in one place.
//
// This exact delegate was written out three times -- in BusyOverlay, in
// TransferProgressFrame, and inline in StickBackupPage -- with the same
// height, the same radius, the same colours and the same 1100 ms sweep.
// Three copies of a thing nobody would ever remember to change together.
//
// The custom background and contentItem are not decoration. Material's
// own delegates are a fixed 4 px internally and do not scale just
// because the control's height does, so a plain height override leaves
// a thin bar in a tall box. And ProgressBar's indeterminate
// visualPosition is not meaningful to bind to, so the indeterminate
// state animates a fixed-width segment across the track directly.
ProgressBar {
    id: track

    property real barHeight: 16

    Layout.preferredHeight: track.barHeight
    implicitHeight: track.barHeight

    background: Rectangle {
        implicitHeight: track.barHeight
        radius: height / 2
        // A translucent tint of the text colour rather than
        // Theme.surface. Surface is the background tinted three per
        // cent, which is invisible against a BusyOverlay -- that dims
        // the page with the same background colour at 72% opacity, so a
        // part-filled bar read as a short bar floating in space with no
        // track behind it. This reads on a card and on the dim alike.
        color: Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.14)
        border.color: Theme.borderSubtle
    }

    contentItem: Item {
        implicitHeight: track.barHeight
        clip: true

        Rectangle {
            visible: !track.indeterminate
            height: parent.height
            width: track.visualPosition * parent.width
            radius: height / 2
            color: Theme.accent
        }

        Rectangle {
            visible: track.indeterminate
            width: parent.width * 0.3
            height: parent.height
            radius: height / 2
            color: Theme.accent
            // Only while actually on screen: an animation left running
            // behind a hidden overlay wakes the render thread forever.
            SequentialAnimation on x {
                running: track.indeterminate && track.visible
                loops: Animation.Infinite
                NumberAnimation {
                    from: -parent.width * 0.3
                    to: parent.width
                    duration: 1100
                    easing.type: Easing.InOutQuad
                }
            }
        }
    }
}
