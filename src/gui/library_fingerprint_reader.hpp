// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <optional>

#include "application/ports/cancellation_token.hpp"
#include "domain/library_fingerprint.hpp"

namespace seabass::gui
{

// The content identity of the library on a stick, read through
// LibraryCatalogCache from whichever of the two catalogs exist
// (rekordboxPath: the PIONEER folder; enginePath: the Engine Library
// folder; either may be empty). A path is given only for a catalog that
// is there, so nullopt whenever one that was given could not be read
// (or the read was cancelled), and when neither was given: a fingerprint
// of the other catalog alone would describe half the library, and read
// as "a different library" about a stick nobody changed. The callers
// then fall back to the hardware identifier and the label.
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
//
// cancel: handed to the cache, which checks it while this waits for a
// pass another thread is running and in the passes this call runs
// itself. A cancelled read returns nullopt.
enum class FingerprintPass
{
    Tracks,
    Cues,
};
std::optional<domain::LibraryFingerprint> readLibraryFingerprint(
    const QString &rekordboxPath, const QString &enginePath, FingerprintPass pass = FingerprintPass::Cues,
    application::CancellationToken cancel = application::CancellationToken::none());

// What the advisor keeps once its second, Cues, read of a stick is back:
// that read's fingerprint when there is one and it knows its cues;
// otherwise the first read's, cuesKnown still false, so the verdict it
// gave stands, still marked as waiting on the cues. A second read that
// failed (a stick pulled between the two, a catalog briefly unreadable)
// returns nothing, since readLibraryFingerprint() never answers with
// half a library. No comparison with the first read: the Full stage may
// have filled in a length between the two, and a probed length is not
// part of a track's identity, but a second read that differs for any
// reason is still the newer word on the stick.
std::optional<domain::LibraryFingerprint> fingerprintAfterCuesPass(const std::optional<domain::LibraryFingerprint> &first,
                                                                   const std::optional<domain::LibraryFingerprint> &second);

// The same, but guaranteed to have read the catalogs rather than a cached
// copy of them. For the one caller whose answer is written down and
// compared against later -- a backup's manifest header -- where a stale
// number is indistinguishable from a true one and turns into "this is a
// different library" about a stick that never changed.
std::optional<domain::LibraryFingerprint> readLibraryFingerprintUncached(const QString &rekordboxPath,
                                                                          const QString &enginePath);

}  // namespace seabass::gui
