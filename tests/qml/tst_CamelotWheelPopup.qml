// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The wheel is two rings of twelve segments a seam apart, and what the
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

    function test_theRingsAreOneSeamApart() {
        const w = opened().wheel;
        verify(w.majorOuter > 0, "the wheel has a size");
        compare(w.majorInner - w.minorOuter, w.seam, "the rings are one seam apart, like the segments");
        if (typeof screenshotDir !== "undefined" && screenshotDir && screenshotDir.length > 0) {
            const popup = w.parent.parent.parent;
            grabImage(popup).save(screenshotDir + "/camelot-wheel.png");
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

    // Every legend tip is shown whole: the info area is sized to the
    // tallest of them (five lines at this width), so none is elided, and
    // the wheel does not move while they come and go.
    function test_everyLegendTipFitsWithoutEliding() {
        const t = opened();
        const info = findChild(t.popup.contentItem, "wheelHoverInfo");
        verify(info !== null);
        waitForRendering(t.popup.contentItem);
        const wheelY = t.wheel.mapToItem(t.popup.contentItem, 0, 0).y;
        const entries = t.popup.legendEntries;
        verify(entries.length >= 5);
        for (let i = 0; i < entries.length; ++i) {
            t.popup.showInfo(entries[i].tip, true);
            waitForRendering(t.popup.contentItem);
            compare(info.text, entries[i].tip);
            verify(!info.truncated, "'" + entries[i].label + "' is shown whole, not elided");
            verify(info.lineCount <= 6, entries[i].label + " takes " + info.lineCount + " lines");
            verify(info.contentHeight <= info.height + 0.5, entries[i].label + " fits the row");
            compare(t.wheel.mapToItem(t.popup.contentItem, 0, 0).y, wheelY, "the wheel stays put");
            if (screenshotDir && screenshotDir.length > 0 && entries[i].label === "Relative") {
                grabImage(t.popup.contentItem).save(screenshotDir + "/camelot-wheel-popup-tip.png");
            }
            t.popup.showInfo(entries[i].tip, false);
        }
    }

    // A seam belongs to the nearer segment, so crossing one never reads
    // as "nothing": not between neighbours, not between the rings.
    function test_theSeamsAreNotHoles() {
        const t = opened();
        const gapR = (t.wheel.minorOuter + t.wheel.majorInner) / 2;
        const a = -90 * Math.PI / 180;
        compare(t.pointer.segmentAt(t.wheel.cx + (gapR + 1) * Math.cos(a), t.wheel.cy + (gapR + 1) * Math.sin(a)).minor,
                false, "just outside the ring gap is the major ring");
        compare(t.pointer.segmentAt(t.wheel.cx + (gapR - 1) * Math.cos(a), t.wheel.cy + (gapR - 1) * Math.sin(a)).minor,
                true, "just inside it is the minor ring");
        verify(t.pointer.segmentAt(t.wheel.cx + gapR * Math.cos(a), t.wheel.cy + gapR * Math.sin(a)).index >= 0,
               "the gap itself is a key");
    }

    // Clicking a segment picks it: it stays lifted and its text stays in
    // the info area after the pointer leaves; clicking it again lets go.
    function test_clickingPinsASegment() {
        const t = opened();
        const info = findChild(t.popup.contentItem, "wheelHoverInfo");
        const c = centreOf(t.wheel, 5, false);
        mouseClick(t.pointer, c.x, c.y);
        compare(t.wheel.selectedIndex, 4, "5B is picked");
        compare(t.wheel.selectedMinor, false);
        mouseMove(t.pointer, t.wheel.cx, t.wheel.cy);
        tryVerify(() => info.text.indexOf(t.popup.wedgeLabel(5, false)) === 0, 1000,
                  "the picked key stays in the info area: '" + info.text + "'");
        mouseClick(t.pointer, c.x, c.y);
        compare(t.wheel.selectedIndex, -1, "a second click lets go");
        mouseMove(t.pointer, t.wheel.cx, t.wheel.cy);
        tryCompare(info, "text", "", 1000, "and the info area empties");
    }

    // Dragged by its title bar, the popup moves as far as the pointer
    // does. It used to measure the pointer in the title bar's own
    // coordinates, which move with the popup, and trailed at half pace.
    function test_draggingFollowsThePointer() {
        const t = opened();
        const bar = findChild(t.popup.contentItem, "wheelTitleBar");
        verify(bar !== null);
        const x0 = t.popup.x;
        const y0 = t.popup.y;
        const px = 20;
        const py = bar.height / 2;
        mousePress(bar, px, py);
        for (let i = 1; i <= 10; ++i) {
            // Where the pointer is now, in the bar's coordinates as they
            // are after the popup has followed the last step.
            mouseMove(bar, px + 6 * i - (t.popup.x - x0), py + 3 * i - (t.popup.y - y0));
        }
        mouseRelease(bar, px + 60 - (t.popup.x - x0), py + 30 - (t.popup.y - y0));
        compare(Math.round(t.popup.x - x0), 60, "across as far as the pointer went");
        compare(Math.round(t.popup.y - y0), 30, "and down");
    }
}
