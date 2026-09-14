// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// Breeze's "go-home" icon: the breadcrumb's way back to the start.
//
// It used to be the icon's path data drawn as a Shape, because the two
// ways of using the .svg itself both failed here: Qt's SVG renderer does
// not resolve Breeze's fill:currentColor (the icon came out near-black),
// and tinting with MultiEffect draws nothing under software rendering or
// in the offscreen screenshot harness. SeabassIcon tints on the CPU
// (ColorImage), which takes the shape's alpha and nothing else, so
// neither applies and the bundled file can be used like every other icon.
SeabassIcon {
    iconName: "go-home"
    size: Theme.iconSizeSmall * 0.7
}
