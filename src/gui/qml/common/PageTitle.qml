// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// Shared page/section title -- see docs/design/type-scale.html for the
// typography decision this implements ("Quiet" pairing). Replaces the
// font.bold/font.pointSize pair that used to be repeated in every page
// header. Titles are never bold; "page" is for the single top-level
// screen (StickListPage), "section" is every page reached via the back
// chevron.
Label {
    id: root
    property string level: "section"  // "page" | "section" | "crumb"

    // Theme's ink, not the palette's: under a style that keeps its own
    // palette (ControlsStyle.inksFromPalette false) that palette does not
    // follow Theme, and with the system theme on a light session a title
    // with no colour of its own was drawn in the style's light ink on
    // Theme's light background -- 1.10:1 in tst_InkContrast.
    color: Theme.text
    font.family: Theme.titleFamily
    font.weight: Theme.titleWeight
    font.pointSize: root.level === "page" ? Theme.titleLarge
        : root.level === "crumb" ? Theme.titleCrumb
        : Theme.titleMedium
    elide: Text.ElideRight
}
