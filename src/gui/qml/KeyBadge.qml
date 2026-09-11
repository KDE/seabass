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
    Layout.preferredWidth: 50 * Theme.iconScale
    Layout.preferredHeight: 22 * Theme.iconScale
    Layout.alignment: Qt.AlignVCenter
    implicitWidth: 50 * Theme.iconScale
    implicitHeight: 22 * Theme.iconScale

    // A slight "pushed in" shrink while either MouseArea below is
    // actually held down -- the only click feedback a badge gives now
    // that hovering no longer changes the cursor (see fallbackHover/
    // keyHover's own comments).
    scale: (fallbackHover.pressed || keyHover.pressed) ? 0.88 : 1.0
    Behavior on scale { NumberAnimation { duration: 80; easing.type: Easing.OutQuad } }

    readonly property string camelot: Theme.camelotLabel(root.keyName)
    readonly property string badgeLabel: root.notation === "traditional"
        ? Theme.traditionalLabel(root.keyName) : root.camelot
    // Always the full spoken form regardless of notation -- the whole
    // point of hovering is "how do I actually say this," which the
    // short badge label (either "6A" or "F♯m") doesn't spell out on its
    // own.
    readonly property string spokenLabel: Theme.traditionalSpokenLabel(root.keyName)

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
            onClicked: wheelPopup.openAt("")
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.camelot.length > 0
        radius: height / 2
        color: keyHover.containsMouse
            ? Qt.lighter(Theme.colorForKey(root.keyName), 1.2)
            : Theme.colorForKey(root.keyName)

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
            ToolTip.text: "Key: " + root.spokenLabel
                + (root.notation === "traditional" ? " (" + root.camelot + ")" : "")
                + (root.additionalText.length > 0 ? "\n" + root.additionalText : "")
            onClicked: wheelPopup.openAt(root.camelot)
        }
    }

    CamelotWheelPopup {
        id: wheelPopup
        notation: root.notation
        onKeyHovered: (number, isMinor, hovering) => root.keyHovered(number, isMinor, hovering)
        onRelationHovered: (relationLabel, hovering) => root.relationHovered(relationLabel, hovering)
    }
}
