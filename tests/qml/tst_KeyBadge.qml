// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Key badges share one Camelot wheel (CamelotWheelHost.qml): none is made
// until a badge is clicked, a second badge reuses the first one's wheel
// rather than making its own, and the wheel's hovers reach the badge that
// opened it, not another one. A badge used to carry a wheel of its own,
// made with the badge, which is what a list of tracks paid for per row.
TestCase {
    id: testCase
    name: "KeyBadge"
    when: windowShown
    // Clicked, so shown: a TestCase is not visible by itself.
    visible: true
    width: 600
    height: 640

    Component {
        id: badgeComponent
        KeyBadge {}
    }

    SignalSpy { id: firstHovers; signalName: "keyHovered" }
    SignalSpy { id: secondHovers; signalName: "keyHovered" }

    function clickBadge(badge) {
        mouseClick(badge, badge.width / 2, badge.height / 2);
    }

    function test_badgesShareOneWheelMadeOnTheFirstClick() {
        const first = createTemporaryObject(badgeComponent, testCase, {keyName: "Am", x: 20, y: 20});
        const second = createTemporaryObject(badgeComponent, testCase, {keyName: "C", x: 120, y: 20});
        verify(first !== null && second !== null);
        waitForRendering(second);
        firstHovers.target = first;
        secondHovers.target = second;
        firstHovers.clear();
        secondHovers.clear();

        // Made by a click, not by the badge: a previous case in this run
        // may have left one behind, so the count is of wheels this click
        // made, and none of them is open yet.
        verify(CamelotWheelHost.wheel === null || !CamelotWheelHost.wheel.opened,
               "no wheel is open before any badge is clicked");

        clickBadge(first);
        verify(CamelotWheelHost.wheel !== null, "the first click makes the wheel");
        const wheel = CamelotWheelHost.wheel;
        tryVerify(() => wheel.opened, 2000, "and opens it");
        compare(CamelotWheelHost.opener, first, "for the badge that was clicked");

        wheel.close();
        tryVerify(() => !wheel.visible, 2000);
        clickBadge(second);
        verify(CamelotWheelHost.wheel === wheel, "the second badge opens the same wheel, not one of its own");
        tryVerify(() => wheel.opened, 2000);
        compare(CamelotWheelHost.opener, second, "now for the second badge");

        // The wheel's hovers go to whichever badge has it open.
        wheel.keyHovered(8, true, true);
        compare(secondHovers.count, 1, "the badge that opened the wheel hears its hover");
        compare(firstHovers.count, 0, "the other one does not");
        compare(secondHovers.signalArguments[0][0], 8);
        compare(secondHovers.signalArguments[0][1], true);

        wheel.close();
        tryVerify(() => !wheel.visible, 2000);
        compare(CamelotWheelHost.opener, null, "closed, it belongs to no badge");
    }
}
