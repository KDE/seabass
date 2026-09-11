// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma Singleton
import QtQuick

// Remembers where the user last dragged a Camelot Wheel popup to,
// shared across every KeyBadge's own popup instance (there's one per
// badge -- see KeyBadge.qml) so reopening the wheel from a different
// track's key still comes back to where you left it, as long as the
// window hasn't changed size since (see forWidth/forHeight below, when
// the old position might not even be on screen any more). In-memory
// only for this run of the app -- not written to QSettings, doesn't
// need to survive a restart.
QtObject {
    property real x: -1  // -1 = never dragged; popup falls back to its own centered default
    property real y: -1
    property real forWidth: 0
    property real forHeight: 0
}
