// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma Singleton
import QtQuick

// Single source of truth for the display name of each catalog format
// this app reads/writes -- "rekordbox"/"engine"/"onelibrary" internally
// (format strings, property names like hasRekordbox/rekordboxPath stay
// as they are; this is user-facing text only). "DeviceLibrary" (not
// "Rekordbox") specifically for the per-device rekordbox USB export --
// rekordbox is also the name of the *other* format this app reads
// (OneLibrary), so labelling only one of the two "Rekordbox" reads as
// if it were the only rekordbox-branded thing here, which it isn't.
// Originally three near-identical local formatLabel() copies lived in
// SyncPage.qml/DuplicatesPage.qml/CleanupPage.qml -- pulled out once it
// was clear they'd drifted (SyncPage.qml's own had already been fixed
// to say "DeviceLibrary", the other two still said "Rekordbox").
QtObject {
    function label(format) {
        if (format === "engine") return "Engine";
        if (format === "onelibrary") return "OneLibrary";
        // Not a catalog at all: an audio file on the stick that no
        // catalog references (domain::Track::isUnreferenced). It reaches
        // the same track lists as the three formats, and falling through
        // to "DeviceLibrary" below would claim a catalog entry that does
        // not exist -- on a page whose next step is deleting the file.
        if (format === "disk") return "Not in any catalog";
        return "DeviceLibrary";
    }
}
