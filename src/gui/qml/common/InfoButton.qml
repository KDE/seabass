// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import SeabassGui

// A small "?" button that opens a popup with an explanation, in place of
// pointing someone at a file in the source tree they don't have (e.g. a
// docs/*.md) -- anything a person running the built app needs to
// understand should be explained inside the app itself.
ToolButton {
    id: root
    required property string explanationTitle
    // One or two sentences answering "what is this page for". Shown
    // first and emphasised, so the popup can be understood without
    // reading the body -- most people want the summary and leave.
    property string summaryText: ""
    // The body. Markdown: "## " for a section heading, "- " for a
    // bullet, blank line between paragraphs. Prose in one long block is
    // what these grew into, and nobody reads it.
    required property string explanationText

    text: "?"
    implicitWidth: 28
    implicitHeight: 28

    ToolTip.visible: hovered
    ToolTip.text: "More info"

    onClicked: popup.open()

    Popup {
        id: popup

        // Centred on the button, then pulled back inside the window if
        // that would push it past an edge. This button is usually the
        // last thing on a header row, i.e. hard against the window's
        // right edge, where centring alone put most of the text outside
        // the window -- silently clipped, with no scrollbar and no hint
        // that anything was missing.
        readonly property int edgeMargin: 8

        // Positioned imperatively when it is about to show, not by a
        // binding on the window's size.
        //
        // Two earlier attempts bound x/width to root.Window.width and
        // then to Overlay.overlay.width. Both are null while the Popup
        // is being constructed, and neither notifies when it later
        // becomes real, so the binding kept its first value: the
        // arithmetic stayed NaN, Qt ignored it, and the popup went on
        // running off the right edge exactly as before. Both times it
        // looked fixed in the source and was not -- caught on a
        // screenshot, not by reading it. By onAboutToShow the popup has
        // a window, so the numbers are simply there to be read.
        function placeInsideWindow() {
            var overlay = Overlay.overlay;
            var w = overlay ? overlay.width : (root.Window.width || 0);
            var h = overlay ? overlay.height : (root.Window.height || 0);
            if (w > 0) {
                width = Math.min(420, w - 2 * edgeMargin);
            }
            var centered = Math.round((root.width - width) / 2);
            var sceneLeft = root.mapToItem(null, centered, 0).x;
            if (w > 0) {
                var maxLeft = w - width - edgeMargin;
                if (sceneLeft > maxLeft) {
                    centered -= (sceneLeft - maxLeft);
                    sceneLeft = maxLeft;
                }
                if (sceneLeft < edgeMargin) {
                    centered += (edgeMargin - sceneLeft);
                }
            }
            x = centered;
            // Vertical fit, not just horizontal. Anchored below the
            // button there is only as much room as the button's distance
            // from the bottom, and this button often sits well down a
            // page -- so the popup was clipped by the window edge with
            // no hint anything was missing, the same failure the
            // horizontal case above already fixed.
            var top = root.mapToItem(null, 0, 0).y;
            var below = top + root.height;
            var roomBelow = h > 0 ? h - below - edgeMargin : 600;
            if (h > 0 && roomBelow < Math.min(260, h - 2 * edgeMargin)) {
                // Not enough room under the button: sit near the top of
                // the window instead and use its full height.
                maxHeight = Math.max(120, h - 2 * edgeMargin);
                y = edgeMargin - top;
            } else {
                maxHeight = Math.max(120, roomBelow);
                y = root.height;
            }
        }

        property real maxHeight: 600
        onAboutToShow: placeInsideWindow()

        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.surface
            border.color: Theme.textMuted
            radius: 6
        }

        // Everything here addresses its siblings by id, never through
        // parent/parent.parent: the Popup's parent chain does not exist
        // yet while its contentItem is being constructed, and reaching
        // through it made object creation fail -- which main.cpp turns
        // into a silent exit(-1), no window and no QML error printed.
        contentItem: ScrollView {
            id: scroller
            clip: true
            contentWidth: availableWidth
            implicitHeight: Math.min(column.implicitHeight, popup.maxHeight)
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: column
                width: scroller.availableWidth
                spacing: 8

                Label {
                    text: root.explanationTitle
                    font.bold: true
                    font.pointSize: Theme.fontMedium
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                // The summary, set apart so the eye lands on it first.
                Label {
                    visible: root.summaryText.length > 0
                    text: root.summaryText
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.fontMedium
                    color: Theme.text
                    padding: 10
                    Layout.fillWidth: true
                    background: Rectangle {
                        color: Theme.groupBackground
                        border.color: Theme.borderSubtle
                        border.width: 1
                        radius: 4
                    }
                }
                Label {
                    text: root.explanationText
                    // Markdown so the body can carry headings and
                    // bullets. Same reason the summary exists: a wall of
                    // prose in a popup does not get read.
                    textFormat: Text.MarkdownText
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
            }
        }
    }
}
