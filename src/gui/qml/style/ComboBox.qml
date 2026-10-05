// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Templates as T
import SeabassGui

// The whole control, popup included. The popup is the half that matters:
// what a style draws there comes from the PLATFORM's palette, while every
// colour here comes from Theme, which is dark unless the user asked for
// the system one. Where the two disagree the entries are drawn on a
// ground Theme never chose, and the list reads as blank -- dark on dark,
// or Theme's near-white ink on the style's white popup. Both have been
// seen in this app on macOS.
T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)
    leftPadding: 8
    rightPadding: 8
    padding: 6

    delegate: ItemDelegate {
        required property var model
        required property int index
        width: ListView.view ? ListView.view.width : control.width
        text: model[control.textRole] !== undefined ? model[control.textRole] : model.display
        highlighted: control.highlightedIndex === index
        hoverEnabled: control.hoverEnabled
    }

    indicator: SeabassIcon {
        x: control.mirrored ? control.padding : control.width - width - control.padding
        y: Theme.snap(control.topPadding + (control.availableHeight - height) / 2)
        iconName: "arrow-down"
        size: Theme.iconSizeSmall * 0.5
        color: Theme.textMuted
    }

    contentItem: T.TextField {
        leftPadding: 0
        rightPadding: control.indicator ? control.indicator.width + control.spacing : 0
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        font: control.font
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentInk
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: Theme.compactControlHeight
        radius: Theme.cornerRadius
        color: control.hovered ? Theme.rowHover : Theme.surface
        border.color: control.visualFocus ? Theme.accent : Theme.border
        border.width: 1
    }

    popup: T.Popup {
        y: control.height
        width: control.width
        height: Math.min(contentItem.implicitHeight + 2, control.Window.height - topMargin - bottomMargin)
        topMargin: 6
        bottomMargin: 6
        padding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            T.ScrollIndicator.vertical: T.ScrollIndicator {}
        }

        background: Rectangle {
            radius: Theme.cornerRadius
            color: Theme.surface
            border.color: Theme.border
            border.width: 1
        }
    }
}
