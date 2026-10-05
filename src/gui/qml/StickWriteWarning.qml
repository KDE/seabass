// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// Shown while a controller is actively writing to the stick (not merely
// scanning it) -- unplugging mid-write can corrupt the very file being
// written. Collapses to zero height when not visible, so pages can leave
// it in their layout unconditionally.
WarningBanner {
    visible: false
    text: "Writing to the stick. Do not remove it until this finishes."
}
