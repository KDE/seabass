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
//
// pass: how far to read. Tracks is the catalog files alone, a fraction
// of a second on a stick: the tracks and the playlists, and the cues of
// a catalog that keeps them inside (Engine). rekordbox keeps its cues in
// one ANLZ file per track, so a Tracks read of a stick with a rekordbox
// catalog comes back with cuesKnown false. Cues reads those too: the
// whole fingerprint, for as long as the Cues stage takes.
enum class FingerprintPass
{
    Tracks,
    Cues,
};
std::optional<domain::LibraryFingerprint> readLibraryFingerprint(const QString &rekordboxPath, const QString &enginePath,
                                                                 FingerprintPass pass = FingerprintPass::Cues);

// The same, but guaranteed to have read the catalogs rather than a cached
// copy of them. For the one caller whose answer is written down and
// compared against later -- a backup's manifest header -- where a stale
// number is indistinguishable from a true one and turns into "this is a
// different library" about a stick that never changed.
std::optional<domain::LibraryFingerprint> readLibraryFingerprintUncached(const QString &rekordboxPath,
                                                                          const QString &enginePath);

}  // namespace seabass::gui
