// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick

// The heart's beat: two small swells with a slight brightening on each,
// on sine curves (an organic rise and settle, no snap), the second a
// touch weaker, then a rest.
//
// Extracted from the Support button in StickListPage.qml's header so the
// page behind that button can beat with the same movement at a different
// rate: the button's heart is a subtle living detail at the edge of the
// eye and rests about six seconds between beats, while the page's heart
// is what the page is about and beats every three.
//
// Drives scale and opacity on the target directly rather than through a
// Behavior, which would re-trigger on every intermediate value this same
// animation produces.
SequentialAnimation {
    id: beat

    // The item that swells. Its scale and opacity are animated.
    property Item target: null
    // One beat plus its rest, in milliseconds.
    property int period: 6230
    // How quiet the target sits between beats.
    property real restingOpacity: 0.85

    // The beat keeps its shape at any rate: the original durations, all
    // scaled by one factor, with the rest taking whatever is left. The
    // beat is given at most 45% of the cycle -- about what a heart at
    // rest spends contracting -- so a fast rate shortens the movement
    // rather than eating its own rest and reading as a continuous throb.
    readonly property real _shape: 1430  // 260 + 340 + 90 + 220 + 520
    readonly property real _squeeze: Math.min(1, (period * 0.45) / _shape)
    function _ms(base) { return Math.max(1, Math.round(base * beat._squeeze)); }

    running: target !== null
    loops: Animation.Infinite

    ParallelAnimation {
        NumberAnimation { target: beat.target; property: "scale"; to: 1.12; duration: beat._ms(260); easing.type: Easing.OutSine }
        NumberAnimation { target: beat.target; property: "opacity"; to: 1.0; duration: beat._ms(260); easing.type: Easing.OutSine }
    }
    ParallelAnimation {
        NumberAnimation { target: beat.target; property: "scale"; to: 1.0; duration: beat._ms(340); easing.type: Easing.InOutSine }
        NumberAnimation { target: beat.target; property: "opacity"; to: beat.restingOpacity; duration: beat._ms(340); easing.type: Easing.InOutSine }
    }
    PauseAnimation { duration: beat._ms(90) }
    ParallelAnimation {
        NumberAnimation { target: beat.target; property: "scale"; to: 1.07; duration: beat._ms(220); easing.type: Easing.OutSine }
        NumberAnimation { target: beat.target; property: "opacity"; to: 1.0; duration: beat._ms(220); easing.type: Easing.OutSine }
    }
    ParallelAnimation {
        NumberAnimation { target: beat.target; property: "scale"; to: 1.0; duration: beat._ms(520); easing.type: Easing.InOutSine }
        NumberAnimation { target: beat.target; property: "opacity"; to: beat.restingOpacity; duration: beat._ms(520); easing.type: Easing.InOutSine }
    }
    // Whatever the beat did not use. The Math.max is what keeps this
    // non-negative, not the 45% cap: _ms() floors every segment at 1 ms,
    // so below a period of about 18 ms the five segments sum to more than
    // the period and the cap stops holding. No caller is anywhere near
    // that -- the shortest is 3000 -- but the guarantee lives here.
    PauseAnimation {
        duration: Math.max(0, beat.period - (beat._ms(260) + beat._ms(340) + beat._ms(90)
                                             + beat._ms(220) + beat._ms(520)))
    }
}
