// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <optional>

#include "domain/library_fingerprint.hpp"

namespace seabass::gui
{

// The content identity of the library on a stick, read through
// LibraryCatalogCache from whichever of the two catalogs exist
// (rekordboxPath: the PIONEER folder; enginePath: the Engine Library
// folder; either may be empty). nullopt when neither could be read: the
// callers then fall back to the hardware identifier and the label.
// Read-only, safe on a worker thread; shared by the stick backup, the
// advisor and the clone controller so all three agree on what "this
// library" means.
std::optional<domain::LibraryFingerprint> readLibraryFingerprint(const QString &rekordboxPath, const QString &enginePath);

}  // namespace seabass::gui
