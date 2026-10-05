// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// A tooltip is a box with text in it, and the box has a size.
//
// SeabassStyle's first ToolTip set no implicitWidth or implicitHeight. A
// template computes none of its own, so the popup was 0 x 0: its
// background, sized to the popup, was not drawn at all, while the text
// still painted at its full natural width -- no box behind it, and running
// off the edge of the window. Nothing failed; the app logged nothing.
TestCase {
    id: testCase
    name: "ToolTipStyle"
    when: windowShown
    width: 600
    height: 400

    Button {
        id: target
        x: 20
        y: 20
        text: "Hover me"
    }

    function shown(text) {
        target.ToolTip.show(text);
        const tip = target.ToolTip.toolTip;
        tryVerify(() => tip.opened, 2000, "the tooltip opens");
        waitForRendering(target);
        return tip;
    }

    function cleanup() {
        target.ToolTip.hide();
    }

    function test_theBoxHasASize() {
        const tip = shown("A short tip");
        verify(tip.width > 0 && tip.height > 0,
               "tooltip is " + tip.width + " x " + tip.height + "; a template that sets no size draws no box");
        verify(tip.background !== null, "it has a background");
        compare(Math.round(tip.background.width), Math.round(tip.width), "the background covers the whole tooltip");
        compare(Math.round(tip.background.height), Math.round(tip.height));
    }

    function test_theTextIsInsideTheBox() {
        const tip = shown("A short tip");
        verify(tip.contentItem.width <= tip.width,
               "text " + tip.contentItem.width + " px wide in a " + tip.width + " px tooltip");
    }

    // A long tip wraps into a block instead of being one line as wide as
    // its text, which is what ran off the window.
    function test_aLongTipWrapsInsteadOfRunningOffTheWindow() {
        const long = "This sentence is deliberately far longer than any tooltip "
            + "ought to be on one line, so that it has to wrap onto several "
            + "lines to stay inside the window it was opened in.";
        // Measured against a one-line tip of the same style rather than
        // through contentItem.font: KDE's desktop style gives its tooltip
        // a contentItem with no font property.
        const oneLineHeight = shown("Short").height;
        target.ToolTip.hide();
        const tip = shown(long);
        verify(tip.width < testCase.width,
               "tooltip " + tip.width + " px wide in a " + testCase.width + " px window");
        verify(tip.height > oneLineHeight * 1.5,
               "a long tip wraps onto more than one line: " + tip.height + " px tall, one line is "
               + oneLineHeight + " px");
    }
}
