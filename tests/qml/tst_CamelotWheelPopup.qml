// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The wheel is two rings of twelve segments that touch, and what the
// pointer is over is told in the info area top right rather than in a
// tooltip laid over the wheel.
TestCase {
    id: testCase
    name: "CamelotWheelPopup"
    when: windowShown
    width: 600
    height: 640

    Component {
        id: wheelComponent
        CamelotWheelPopup {
            originNumber: 8
            originIsMinor: false
        }
    }

    function opened() {
        const popup = createTemporaryObject(wheelComponent, testCase);
        verify(popup !== null);
        popup.open();
        tryVerify(() => popup.opened, 2000);
        const pointer = findChild(popup.contentItem, "wheelPointer");
        verify(pointer !== null, "the wheel has one pointer target");
        waitForRendering(pointer);
        return {popup: popup, pointer: pointer, wheel: pointer.parent};
    }

    // Where a key's segment is: its centre, in the wheel's own coordinates.
    function centreOf(wheel, number, minor) {
        const inner = minor ? wheel.minorInner : wheel.majorInner;
        const outer = minor ? wheel.minorOuter : wheel.majorOuter;
        const r = (inner + outer) / 2;
        const a = ((number - 1) * 30 - 90) * Math.PI / 180;
        return {x: wheel.cx + r * Math.cos(a), y: wheel.cy + r * Math.sin(a)};
    }

    function test_theRingsTouch() {
        const w = opened().wheel;
        verify(w.majorOuter > 0, "the wheel has a size");
        compare(w.minorOuter, w.majorInner, "the minor ring ends where the major ring begins");
        if (typeof screenshotDir !== "undefined" && screenshotDir && screenshotDir.length > 0) {
            grabImage(w.parent).save(screenshotDir + "/camelot-wheel.png");
        }
    }

    // Every key, both rings: the point at a segment's centre is that key.
    function test_everySegmentAnswersForItsOwnKey() {
        const t = opened();
        for (var n = 1; n <= 12; ++n) {
            for (const minor of [false, true]) {
                const c = centreOf(t.wheel, n, minor);
                const hit = t.pointer.segmentAt(c.x, c.y);
                compare(hit.index, n - 1, (n + (minor ? "A" : "B")) + ": wrong segment");
                compare(hit.minor, minor, (n + (minor ? "A" : "B")) + ": wrong ring");
            }
        }
    }

    // Just either side of a seam is the key on that side, not its neighbour.
    function test_aSeamSeparatesNeighbours() {
        const t = opened();
        const r = (t.wheel.majorInner + t.wheel.majorOuter) / 2;
        // The seam between 1B (centred at -90 degrees) and 2B (-60) is at -75.
        for (const [deg, expected] of [[-76, 0], [-74, 1]]) {
            const a = deg * Math.PI / 180;
            const hit = t.pointer.segmentAt(t.wheel.cx + r * Math.cos(a), t.wheel.cy + r * Math.sin(a));
            compare(hit.index, expected, "at " + deg + " degrees");
        }
    }

    function test_theCentreAndOutsideAreNoKey() {
        const t = opened();
        compare(t.pointer.segmentAt(t.wheel.cx, t.wheel.cy).index, -1, "the hole in the middle");
        compare(t.pointer.segmentAt(t.wheel.cx + t.wheel.majorOuter + 10, t.wheel.cy).index, -1, "outside the wheel");
    }

    function test_hoveringTellsTheInfoAreaNotATooltip() {
        const t = opened();
        const info = findChild(t.popup.contentItem, "wheelHoverInfo");
        verify(info !== null);
        compare(info.text, "", "nothing hovered, nothing said");
        const c = centreOf(t.wheel, 3, true);
        mouseMove(t.pointer, c.x, c.y);
        tryVerify(() => info.text.indexOf(t.popup.wedgeLabel(3, true)) === 0, 1000,
                  "the info area names the key under the pointer: '" + info.text + "'");
        mouseMove(t.pointer, t.wheel.cx, t.wheel.cy);
        tryCompare(info, "text", "", 1000, "leaving the segments clears it");
    }
}
