// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A musical key rendered as a small colored pill, hue picked by its
// Camelot wheel position (see Theme.colorForKey()'s own doc comment for
// why: wheel-adjacent, harmonically compatible keys land close together
// in color too, not just an arbitrary color-per-key). Empty/unparseable
// keys render as a plain "--", same as the bare-text column this
// replaces. Clicking any badge opens a Camelot Wheel reference popup
// (CamelotWheelPopup.qml), pre-selecting this badge's own key when it's
// a recognized one.
Item {
    id: root
    property string keyName: ""
    // "camelot" (default, e.g. "6A") or "traditional" (e.g. "F♯m") --
    // AppSettingsController.keyNotation, threaded through by every call
    // site rather than read from Theme directly, so this stays testable/
    // previewable without a running AppSettingsController.
    property string notation: "camelot"
    // Extra context appended to the hover tooltip after the key itself --
    // e.g. MatchingPage.qml passing how this key relates to the
    // track it's being matched against ("Energy Boost"). Empty by
    // default; most call sites have nothing extra to say.
    property string additionalText: ""
    // Forwarded straight from the wheel popup's own signals (see
    // CamelotWheelPopup.qml's own doc comment) -- lets a host like
    // MatchingPage.qml react to hovering there without needing its
    // own reference to whichever badge's popup happens to be open.
    signal keyHovered(int number, bool isMinor, bool hovering)
    signal relationHovered(string relationLabel, bool hovering)
    Layout.preferredWidth: Theme.scaled(50)
    Layout.preferredHeight: Theme.scaled(22)
    Layout.alignment: Qt.AlignVCenter
    implicitWidth: Theme.scaled(50)
    implicitHeight: Theme.scaled(22)

    // A slight "pushed in" shrink while either MouseArea below is
    // actually held down -- the only click feedback a badge gives now
    // that hovering no longer changes the cursor (see fallbackHover/
    // keyHover's own comments).
    scale: (fallbackHover.pressed || keyHover.pressed) ? 0.88 : 1.0
    Behavior on scale { NumberAnimation { duration: 80; easing.type: Easing.OutQuad } }

    readonly property string camelot: Theme.camelotLabel(root.keyName)
    readonly property string badgeLabel: root.notation === "traditional"
        ? Theme.traditionalLabel(root.keyName) : root.camelot
    // The badge's colour, worked out once: it was asked for twice per
    // badge and again on every hover, each time parsing the key anew, in
    // every row of every list of tracks.
    readonly property color keyColor: Theme.colorForKey(root.keyName)

    Label {
        anchors.centerIn: parent
        visible: root.keyName.length === 0 || root.camelot.length === 0
        text: root.keyName.length > 0 ? root.keyName : "--"
        color: fallbackHover.containsMouse ? Theme.text : Theme.textMuted

        // No cursor change and no tooltip here -- both read as "this is a
        // link" more than "this shows more info," which wasn't the
        // intent. The hover color shift above and the press animation
        // below are cue enough that this fallback is still clickable.
        MouseArea {
            id: fallbackHover
            anchors.fill: parent
            hoverEnabled: true
            visible: root.keyName.length > 0
            onClicked: root.openWheel("")
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.camelot.length > 0
        radius: height / 2
        color: keyHover.containsMouse ? Qt.lighter(root.keyColor, 1.2) : root.keyColor

        Label {
            anchors.centerIn: parent
            text: root.badgeLabel
            font.bold: true
            font.pointSize: Theme.fontSmall
            color: Theme.contrastingTextColor(parent.color)
        }

        MouseArea {
            id: keyHover
            anchors.fill: parent
            hoverEnabled: true
            ToolTip.visible: containsMouse
            // Always the full spoken form regardless of notation -- the
            // whole point of hovering is "how do I actually say this,"
            // which the short badge label (either "6A" or "F♯m") doesn't
            // spell out on its own. Worked out only while it is shown: it
            // is one more parse of the key, for a tooltip most rows never
            // show.
            ToolTip.text: !containsMouse ? "" : "Key: " + Theme.traditionalSpokenLabel(root.keyName)
                + (root.notation === "traditional" ? " (" + root.camelot + ")" : "")
                + (root.additionalText.length > 0 ? "\n" + root.additionalText : "")
            onClicked: root.openWheel(root.camelot)
        }
    }

    // The app's one wheel (CamelotWheelHost.qml), opened for this badge:
    // it reports its hovers back here while this badge has it open.
    function openWheel(key) {
        CamelotWheelHost.open(root, root.Window.contentItem, key, root.notation);
    }
}
