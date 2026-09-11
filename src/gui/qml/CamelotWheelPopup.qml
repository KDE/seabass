// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A visual Camelot wheel, opened by clicking any KeyBadge: a pure
// reference, not an input -- it shows how every position relates to the
// key it was opened with (outer ring = major/B, inner ring = minor/A,
// matching the real wheel's own layout), Same/Relative/Adjacent/Energy
// mix, color-coded and spelled out on hover, with unrelated keys faded
// so the related ones stand out. Filtering the candidate list is the Add
// or Move Track panel's own key-tier row's job, not this popup's; the
// wheel only ever hovers (see keyHovered/relationHovered below), never
// picks.
Popup {
    id: root
    parent: Overlay.overlay
    // Positioned imperatively (see resetPosition(), called from
    // openAt()) rather than with a live x/y binding -- a binding would
    // fight with dragging every time something it depends on re-evaluates.
    modal: true
    focus: true
    width: 380
    height: 480
    // Still modal (captures input, closes on an outside click) but
    // without the default dim-the-whole-window scrim: the rest of the
    // app -- in particular a candidate list doing its own hover-fade
    // highlighting behind this -- needs to stay at its own true opacity,
    // not additionally darkened by the popup's own overlay on top of it.
    Overlay.modal: Rectangle { color: "transparent" }

    // Falls back to centered when nothing's been dragged yet, or the
    // window's a different size than whatever it was dragged at (an old
    // pixel position could be off-screen, or just not make sense, once
    // the window's been resized).
    function resetPosition() {
        if (CamelotWheelPosition.x >= 0 && CamelotWheelPosition.forWidth === root.parent.width
            && CamelotWheelPosition.forHeight === root.parent.height) {
            root.x = CamelotWheelPosition.x;
            root.y = CamelotWheelPosition.y;
        } else {
            root.x = Math.round((root.parent.width - root.width) / 2);
            root.y = Math.round((root.parent.height - root.height) / 2);
        }
    }

    // The key the popup was opened *with* -- fixed for as long as it's
    // open (there's no more clicking around to explore a different one),
    // so it's both what every wedge's relation highlight is computed
    // against and the one wedge marked with its own origin ring.
    property int originNumber: 0  // 0 = unrecognized/empty key
    property bool originIsMinor: false
    // "camelot" (default, e.g. "8B") or "traditional" (e.g. "C major") --
    // AppSettingsController.keyNotation, threaded down from whichever
    // KeyBadge opened this popup (same convention KeyBadge.qml's own
    // `notation` property already follows).
    property string notation: "camelot"

    // Wedge label in whichever notation the app is currently set to --
    // Theme.traditionalLabel() accepts plain Camelot notation ("8B") as
    // valid input (see parseCamelotKey()'s own camelot-notation branch),
    // so this is just a formatting choice, not a different lookup.
    function wedgeLabel(number, isMinor) {
        var camelot = number + (isMinor ? "A" : "B");
        return root.notation === "traditional" ? Theme.traditionalLabel(camelot) : camelot;
    }

    // For a host (e.g. MatchingPage.qml) that wants to react to
    // hovering here -- highlighting matching rows in a track list while
    // the pointer's over a wedge or a legend swatch. hovering false means
    // the pointer just left that wedge/legend row (the other argument
    // still identifies which one). relationHovered's relationLabel is
    // one of domain::keyRelationLabel()'s own exact strings ("Same key",
    // "Relative major/minor", "Adjacent (harmonic)", "Energy mix").
    // Purely ephemeral, both of them -- this popup never picks anything,
    // see the file's own doc comment above.
    signal keyHovered(int number, bool isMinor, bool hovering)
    signal relationHovered(string relationLabel, bool hovering)

    // camelotLabel is e.g. "8A", or "" when the key that opened this
    // didn't parse (KeyBadge.qml's own fallback/unrecognized-key case),
    // in which case the wheel shows plain, unhighlighted wedges.
    function openAt(camelotLabel) {
        var match = /^(1[0-2]|[1-9])([AB])$/.exec(camelotLabel);
        if (match) {
            root.originNumber = parseInt(match[1], 10);
            root.originIsMinor = match[2] === "A";
        } else {
            root.originNumber = 0;
        }
        root.resetPosition();
        root.open();
    }

    // Mirrors domain::classifyKeyRelation (src/domain/camelot_key.cpp),
    // origin -> wedge order (so "Energy Boost"/"Energy Drop" read as
    // "moving from the key this popup was opened with to this wedge").
    // This widget works purely in wheel-position space -- no track.key
    // strings to parse, every wedge is already a plain number/isMinor
    // pair -- so it's simpler to keep this one small pure function local
    // than to thread a controller reference through KeyBadge.qml (used
    // all over the app) just to reach the C++ version.
    function relationLabel(number, isMinor) {
        if (root.originNumber === 0) {
            return "";
        }
        if (number === root.originNumber && isMinor === root.originIsMinor) {
            return "Same key";
        }
        if (number === root.originNumber) {
            return "Relative major/minor";
        }
        var diff = Math.abs(number - root.originNumber);
        var wheelDistance = Math.min(diff, 12 - diff);
        if (wheelDistance === 1) {
            if (isMinor !== root.originIsMinor) {
                return "Energy mix";
            }
            // Clockwise from the origin to this wedge (wrapping 12 -> 1)
            // is the "up"/boost direction, matching
            // domain::classifyKeyRelation()'s own convention.
            var up = number === (root.originNumber % 12) + 1;
            return up ? "Energy Boost" : "Energy Drop";
        }
        return "Unrelated key";
    }

    // null (rather than a color) for "no highlight" -- Unrelated and "no
    // selection yet" both render as a plain, uncolored ring. Adjacent
    // and Energy mix deliberately use two hues nothing else here is
    // close to (gold vs. red) -- warnIcon/conflictText looked too alike
    // side by side on a small badge border.
    function relationColor(number, isMinor) {
        switch (root.relationLabel(number, isMinor)) {
        case "Same key": return Theme.accent;
        case "Relative major/minor": return Theme.good;
        case "Energy Boost": return Theme.warnIcon;
        case "Energy Drop": return Theme.warnIcon;
        case "Energy mix": return Theme.danger;
        default: return null;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // Drag the popup out of the way by its own title bar only (the
        // padding around "Camelot Wheel", not the wedges or legend below)
        // -- restricted here rather than over the whole popup so hovering
        // the wheel itself gives clean, unambiguous pointer feedback
        // instead of a drag cursor everywhere.
        Item {
            id: titleBar
            Layout.fillWidth: true
            implicitHeight: titleRow.implicitHeight

            MouseArea {
                id: dragArea
                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor

                // Manual drag rather than drag.target: root -- Popup isn't
                // a scene-graph Item (it's a QQuickPopup with its own x/y,
                // not a transform MouseArea's built-in drag machinery can
                // attach to), so drag.target silently did nothing: the
                // cursor changed but the popup never actually moved.
                // mouse.x/mouse.y are always local to this MouseArea,
                // which moves with the popup each time root.x/root.y
                // below are updated, so the delta from the press-time
                // local position stays correct frame to frame.
                property real pressLocalX: 0
                property real pressLocalY: 0
                property real pressPopupX: 0
                property real pressPopupY: 0

                onPressed: (mouse) => {
                    dragArea.pressLocalX = mouse.x;
                    dragArea.pressLocalY = mouse.y;
                    dragArea.pressPopupX = root.x;
                    dragArea.pressPopupY = root.y;
                }
                onPositionChanged: (mouse) => {
                    if (pressed) {
                        root.x = dragArea.pressPopupX + (mouse.x - dragArea.pressLocalX);
                        root.y = dragArea.pressPopupY + (mouse.y - dragArea.pressLocalY);
                    }
                }
                onReleased: {
                    CamelotWheelPosition.x = root.x;
                    CamelotWheelPosition.y = root.y;
                    CamelotWheelPosition.forWidth = root.parent.width;
                    CamelotWheelPosition.forHeight = root.parent.height;
                }
            }

            RowLayout {
                id: titleRow
                anchors.fill: parent
                PageTitle { text: "Camelot Wheel"; level: "section"; Layout.fillWidth: true }
                ToolButton {
                    text: "✕"
                    ToolTip.visible: hovered
                    ToolTip.text: "Close"
                    onClicked: root.close()
                }
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            text: root.originNumber === 0
                ? "A visual reference: color and position show which keys mix well together."
                : "Showing how every key relates to " + root.wedgeLabel(root.originNumber, root.originIsMinor) + "."
        }

        Item {
            id: wheel
            Layout.fillWidth: true
            Layout.fillHeight: true

            readonly property real cx: width / 2
            readonly property real cy: height / 2
            readonly property real outerRadius: Math.min(width, height) / 2 - 26
            readonly property real innerRadius: wheel.outerRadius - 54

            Repeater {
                model: 12
                delegate: Item {
                    id: majorWedge
                    required property int index
                    readonly property int number: index + 1
                    readonly property real angle: (index * 30 - 90) * Math.PI / 180
                    x: wheel.cx + wheel.outerRadius * Math.cos(angle) - width / 2
                    y: wheel.cy + wheel.outerRadius * Math.sin(angle) - height / 2
                    width: 42
                    height: 42

                    readonly property color highlight: root.relationColor(majorWedge.number, false)
                    readonly property bool isOrigin: majorWedge.number === root.originNumber && !root.originIsMinor
                    opacity: root.originNumber !== 0 && !majorWedge.isOrigin
                        && root.relationLabel(majorWedge.number, false) === "Unrelated key" ? 0.6 : 1.0
                    Behavior on opacity { NumberAnimation { duration: Theme.shortTransitionDuration } }

                    // Origin marker -- a plain ring independent of the
                    // relation highlight below, so it stays visible no
                    // matter what's faded.
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -5
                        radius: width / 2
                        color: "transparent"
                        border.width: majorWedge.isOrigin ? 2 : 0
                        border.color: Theme.text
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: Theme.colorForKey(majorWedge.number + "B")
                        border.width: majorWedge.highlight ? 3 : 0
                        border.color: majorWedge.highlight ?? "transparent"
                        Label {
                            anchors.centerIn: parent
                            text: root.wedgeLabel(majorWedge.number, false)
                            font.bold: true
                            font.pointSize: Theme.fontSmall
                            color: Theme.contrastingTextColor(parent.color)
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        ToolTip.visible: containsMouse
                        ToolTip.text: root.wedgeLabel(majorWedge.number, false)
                            + (root.relationLabel(majorWedge.number, false).length > 0
                                ? ": " + root.relationLabel(majorWedge.number, false) : "")
                            + (majorWedge.isOrigin ? " (this track's key)" : "")
                        onEntered: root.keyHovered(majorWedge.number, false, true)
                        onExited: root.keyHovered(majorWedge.number, false, false)
                    }
                }
            }
            Repeater {
                model: 12
                delegate: Item {
                    id: minorWedge
                    required property int index
                    readonly property int number: index + 1
                    readonly property real angle: (index * 30 - 90) * Math.PI / 180
                    x: wheel.cx + wheel.innerRadius * Math.cos(angle) - width / 2
                    y: wheel.cy + wheel.innerRadius * Math.sin(angle) - height / 2
                    width: 38
                    height: 38

                    readonly property color highlight: root.relationColor(minorWedge.number, true)
                    readonly property bool isOrigin: minorWedge.number === root.originNumber && root.originIsMinor
                    opacity: root.originNumber !== 0 && !minorWedge.isOrigin
                        && root.relationLabel(minorWedge.number, true) === "Unrelated key" ? 0.6 : 1.0
                    Behavior on opacity { NumberAnimation { duration: Theme.shortTransitionDuration } }

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -5
                        radius: width / 2
                        color: "transparent"
                        border.width: minorWedge.isOrigin ? 2 : 0
                        border.color: Theme.text
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: Theme.colorForKey(minorWedge.number + "A")
                        border.width: minorWedge.highlight ? 3 : 0
                        border.color: minorWedge.highlight ?? "transparent"
                        Label {
                            anchors.centerIn: parent
                            text: root.wedgeLabel(minorWedge.number, true)
                            font.bold: true
                            font.pointSize: Theme.fontSmall
                            color: Theme.contrastingTextColor(parent.color)
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        ToolTip.visible: containsMouse
                        ToolTip.text: root.wedgeLabel(minorWedge.number, true)
                            + (root.relationLabel(minorWedge.number, true).length > 0
                                ? ": " + root.relationLabel(minorWedge.number, true) : "")
                            + (minorWedge.isOrigin ? " (this track's key)" : "")
                        onEntered: root.keyHovered(minorWedge.number, true, true)
                        onExited: root.keyHovered(minorWedge.number, true, false)
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            // Reduced from 14 -- one more legend entry (Adjacent split
            // into Boost/Drop) needs the room in this popup's fixed
            // 380px width.
            spacing: 9
            Repeater {
                model: [
                    { label: "Same", relationLabel: "Same key", color: Theme.accent,
                        tip: "Identical key. The safest possible transition." },
                    { label: "Relative", relationLabel: "Relative major/minor", color: Theme.good,
                        tip: "Same wheel number, opposite mode (e.g. 8A/8B). A seamless swap between the major "
                            + "and minor version of the same key." },
                    { label: "Boost", relationLabel: "Energy Boost", color: Theme.warnIcon,
                        tip: "One step clockwise around the wheel, same mode. The classic harmonic-mixing move, "
                            + "with a subtle lift in energy." },
                    { label: "Drop", relationLabel: "Energy Drop", color: Theme.warnIcon,
                        tip: "One step counter-clockwise around the wheel, same mode. The classic harmonic-mixing "
                            + "move, with a subtle ease in energy." },
                    { label: "Energy mix", relationLabel: "Energy mix", color: Theme.danger,
                        tip: "One step around the wheel, opposite mode. A bigger mood/energy shift than Adjacent, "
                            + "while staying tonally related." },
                ]
                delegate: RowLayout {
                    id: legendItem
                    required property var modelData
                    spacing: 4
                    Rectangle { width: 10; height: 10; radius: 5; color: legendItem.modelData.color }
                    Label { text: legendItem.modelData.label; font.pointSize: Theme.fontTiny; color: Theme.textMuted }

                    HoverHandler {
                        id: legendHover
                        onHoveredChanged: root.relationHovered(legendItem.modelData.relationLabel, legendHover.hovered)
                    }
                    ToolTip.visible: legendHover.hovered
                    ToolTip.text: legendItem.modelData.tip
                    ToolTip.delay: 300
                }
            }
        }
    }
}
