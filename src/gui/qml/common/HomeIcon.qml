import QtQuick
import QtQuick.Shapes
import SeabassGui

// Breeze's "go-home" icon, drawn as a Shape rather than loaded as an SVG.
//
// The artwork is Breeze's: the path data below is the `d` attribute of
// breeze-icons' actions/22/go-home.svg, verbatim.
//   SPDX-FileCopyrightText: 2014 Uri Herrera <uri_herrera@nitrux.in>
//   SPDX-License-Identifier: LGPL-3.0-or-later
//
// Why not the .svg file itself: Breeze recolors its icons through a CSS
// class and `fill:currentColor`, which Qt's SVG renderer does not
// resolve -- it would come out a fixed near-black, invisible on this
// app's dark surface. Tinting a bundled white copy with MultiEffect does
// work, but only where there is a shader path: under software rendering
// (and in this project's own offscreen screenshot harness) the effect
// draws nothing at all, silently, and this icon is the only way back on
// the six pages whose breadcrumb has no middle segment. A Shape is plain
// scene-graph geometry -- it takes Theme's color directly, follows the
// system light/dark switch with it, and renders the same everywhere.
// Same reasoning UsbStickIcon.qml's own header gives for not using a
// font glyph.
Item {
    id: root
    property color color: Theme.textMuted
    implicitWidth: Theme.iconSizeSmall * 0.7
    implicitHeight: Theme.iconSizeSmall * 0.7

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        // Breeze draws on a 22x22 grid; this scales that to whatever size
        // the breadcrumb asks for.
        scale: Math.min(root.width, root.height) / 22
        transformOrigin: Item.TopLeft

        ShapePath {
            fillColor: root.color
            strokeWidth: 0
            strokeColor: "transparent"
            // The outer outline and the inner contour wind opposite ways,
            // so the house reads as an outline rather than a filled blob.
            fillRule: ShapePath.OddEvenFill
            PathSvg {
                path: "m11 3l-.707031.707031-7.292969 7.292969.707031.707031.292969-.292969"
                    + "v7.585938h1 5 3 5v-1-6.585938l.292969.292969.707031-.707031-3-3v-3h-3"
                    + "l-1.292969-1.292969-.707031-.707031m0 1.414063l6 5.999999v7.585938h-4"
                    + "v-5h-3-1v5h-4v-7.585938l6-5.999999"
            }
        }
    }
}
