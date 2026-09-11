// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// "Eyebrow" table column header (Title / Key / BPM / ...) -- mono,
// uppercase, letter-spaced, muted; never bold. See
// docs/design/type-scale.html.
//
// Takes `label` rather than redeclaring `text`: PageTitle.qml's own
// history showed that `required property string text` on a type that
// already inherits `text` from Label silently shadows it instead of
// promoting it, leaving the real label blank.
Label {
    id: root
    required property string label

    text: root.label.toUpperCase()
    font.family: Theme.dataFamily
    font.pointSize: Theme.tableHeaderSize
    font.letterSpacing: 0.6
    color: Theme.textMuted
}
