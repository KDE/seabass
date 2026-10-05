// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl as Impl
import SeabassGui

T.TextField {
    id: control

    implicitWidth: implicitBackgroundWidth + leftInset + rightInset
                   || Math.max(contentWidth, placeholder.implicitWidth) + leftPadding + rightPadding
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding,
                             placeholder.implicitHeight + topPadding + bottomPadding)
    padding: 6
    leftPadding: 8
    rightPadding: 8

    color: control.enabled ? Theme.text : Theme.textMuted
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentInk
    placeholderTextColor: Theme.textMuted
    verticalAlignment: TextInput.AlignVCenter

    Impl.PlaceholderText {
        id: placeholder
        x: control.leftPadding
        y: control.topPadding
        width: control.width - (control.leftPadding + control.rightPadding)
        height: control.height - (control.topPadding + control.bottomPadding)
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        verticalAlignment: control.verticalAlignment
        elide: Text.ElideRight
        renderType: control.renderType
        visible: !control.length && !control.preeditText && (!control.activeFocus || control.horizontalAlignment !== Qt.AlignHCenter)
    }

    background: Rectangle {
        implicitHeight: Theme.compactControlHeight
        radius: Theme.cornerRadius
        color: Theme.surface
        border.color: control.activeFocus ? Theme.accent : Theme.border
        border.width: 1
    }
}
