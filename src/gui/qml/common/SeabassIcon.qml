// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls.impl
import SeabassGui

// One of the Breeze icons bundled under qml/icons/breeze/, drawn in a
// single flat colour. The app's one way of drawing an icon: not a font
// glyph (emoji fall back to colour fonts, and the symbol fonts are not on
// Windows or macOS) and not a theme lookup (which finds nothing where no
// icon theme is installed). See tools/update-breeze-icons.sh for the set.
//
// ColorImage paints every drawn pixel in `color`, keeping only the
// shape's alpha, so the parts Breeze colours (a red trash can, an accent
// dot) come out in the same tone as the rest. Dim a disabled icon by
// passing a dimmer colour, the same one its label gets.
//
// For a button, use the button's own icon instead: icon.source:
// Theme.iconUrl(name), icon.color from the Theme.
ColorImage {
    id: root
    required property string iconName
    // Width and height, in pixels. The SVG is rasterised at this size
    // (sourceSize, which Qt scales by the screen's pixel ratio), and that
    // is also the Image's implicit size, so a layout gives it exactly
    // this. Set this rather than the item's width or a Layout size: a
    // bitmap rendered smaller and stretched comes out blurred.
    property real size: Theme.iconSizeSmall
    source: root.iconName.length > 0 ? Theme.iconUrl(root.iconName) : ""
    color: Theme.textMuted
    sourceSize.width: Math.ceil(root.size)
    sourceSize.height: Math.ceil(root.size)
    width: root.size
    height: root.size
    fillMode: Image.PreserveAspectFit
}
