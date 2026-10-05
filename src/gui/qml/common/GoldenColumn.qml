// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick

// A column that places itself the way a lone element in an open area is
// placed in this app: centred across its parent, and down at the golden
// section -- the space above it to the space below as 1 : 1.618. The
// true middle reads as low. EmptyState places itself the same way; this
// is for anything else of the kind (Sync's "N cues synced").
//
// A component of its own rather than the same bindings written in
// place: written in place they re-ran as the page was torn down, after
// the column itself had gone, and threw.
Column {
    id: root
    x: root.parent ? Math.round((root.parent.width - root.width) / 2) : 0
    y: root.parent ? Math.round(Math.max(0, root.parent.height - root.height) * (1 - 1 / 1.618)) : 0
}
