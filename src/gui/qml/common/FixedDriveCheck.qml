// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The extra question in front of erasing or overwriting a drive the
// computer does not call removable media (DetectedStick::reportedFixed):
// many external USB SSDs and hard drives call themselves fixed disks, and
// so does a drive Seabass listed only because it holds a DJ library. It
// is most likely the user's stick, but it could as well be a disk they
// keep other things on, so they tick that it is a stick before the
// operation's own confirmation can go ahead.
ColumnLayout {
    id: root

    property string driveName: ""
    property alias checked: box.checked

    spacing: 6

    Label {
        objectName: "fixedDriveExplanation"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "Your computer reports \"" + root.driveName + "\" as a fixed disk, not as a removable drive. "
            + "Many external USB drives do, but so do disks that hold other things."
    }
    SeabassCheckBox {
        id: box
        objectName: "fixedDriveCheckBox"
        Layout.fillWidth: true
        text: "\"" + root.driveName + "\" is an external drive I use as a DJ stick"
    }
}
