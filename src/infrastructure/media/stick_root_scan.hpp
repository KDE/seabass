// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/removable_media_locator.hpp"

namespace seabass::infrastructure::media
{

// Checks a mounted root directory for either format's on-disk signature
// (PIONEER/rekordbox/export.pdb, Engine Library/Database2/m.db) and fills
// in stick.rekordboxPath/enginePath accordingly. Pure std::filesystem, no
// OS-specific device APIs -- shared by every platform's
// RemovableMediaLocator so "what makes a mounted root a rekordbox/Engine
// stick" is defined exactly once.
void scanMountedRoot(const std::string &mountPoint, application::DetectedStick &stick);

}  // namespace seabass::infrastructure::media
