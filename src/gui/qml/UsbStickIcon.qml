// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// What a row in the stick list is, as a flat device icon: a USB stick,
// an SD card, or a folder on this computer. Breeze's drive icons, so it
// matches every other icon in the app (see SeabassIcon).
SeabassIcon {
    id: root
    // BRAINSTORM.md: "show different icon for an SD card for improved
    // clarity" -- best-effort, see DetectedStick::isSdCard's own comment
    // for what "best-effort" means per platform.
    property bool isSdCard: false
    // A library opened from an ordinary directory rather than found on
    // removable media (see MediaController::openFolder): drawn as a
    // folder so the list says at a glance which rows are real hardware.
    // Wins over isSdCard -- a folder is not on a card even when the file
    // it was restored from was.
    property bool isFolder: false
    iconName: root.isFolder ? "folder"
        : (root.isSdCard ? "media-flash-sd-mmc" : "drive-removable-media-usb-pendrive")
}
