// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Shapes
import SeabassGui

// breeze-icons' actions/22/love.svg, its outer contour only. The icon is
// an outline -- an outer and an inner contour -- and the Support button
// wants the heart filled. A Shape rather than the .svg for the reason
// HomeIcon.qml gives: Breeze colours through fill:currentColor, which Qt's
// SVG renderer does not resolve.
Item {
    id: root
    property color color: Theme.danger
    implicitWidth: Theme.iconSizeSmall * 0.7
    implicitHeight: Theme.iconSizeSmall * 0.7

    // Square and centred, so a non-square button still gets a heart of
    // the right shape in its middle.
    Item {
        id: box
        anchors.centerIn: parent
        width: Math.min(root.width, root.height)
        height: width

        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            // Breeze draws on a 22x22 grid.
            scale: box.width / 22
            transformOrigin: Item.TopLeft

            ShapePath {
                fillColor: root.color
                strokeWidth: 0
                strokeColor: "transparent"
                PathSvg {
                    path: "M 7 3 C 4.23858 3 2 5.23858 2 8 C 2 13 9 17 11 19 C 13 17 20 13 20 8 "
                        + "C 20 5.23858 17.76142 3 15 3 C 13.36041 3 11.91181 3.78077 11 5 "
                        + "C 10.08819 3.78077 8.6396 3 7 3 z"
                }
            }
        }
    }
}
