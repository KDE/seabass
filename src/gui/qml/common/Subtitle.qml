// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// Section header within a page's body -- "Theme", "Streaming tracks",
// "Experimental features", "What Seabass does". One step below
// PageTitle, same (non-bold) family so it reads as part of the same
// hierarchy. See docs/design/type-scale.html.
Label {
    font.family: Theme.titleFamily
    font.weight: Theme.titleWeight
    font.pointSize: Theme.subtitleSize
}
