// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The window's page stack: one tool giving way to the next with a quiet
// cross-fade instead of the style's own slide or cut (Sebastian,
// 2026-10-17: "a subtle fade animation when switching tools"). The page
// arriving fades in over the arrival time, the one leaving fades out a
// touch quicker, as things arriving and leaving do elsewhere in the app;
// both ease, so neither is caught starting. Push, pop and replace alike.
StackView {
    id: stack

    // Exposed so a test can shorten the fade; the app keeps Theme's times.
    property int arriveDuration: Theme.arrivalTransitionDuration
    property int leaveDuration: Theme.departureTransitionDuration

    component Arrive: Transition {
        NumberAnimation {
            property: "opacity"
            from: 0
            to: 1
            duration: stack.arriveDuration
            easing.type: Easing.OutCubic
        }
    }
    component Leave: Transition {
        NumberAnimation {
            property: "opacity"
            from: 1
            to: 0
            duration: stack.leaveDuration
            easing.type: Easing.InCubic
        }
    }

    pushEnter: Arrive {}
    pushExit: Leave {}
    popEnter: Arrive {}
    popExit: Leave {}
    replaceEnter: Arrive {}
    replaceExit: Leave {}
}
