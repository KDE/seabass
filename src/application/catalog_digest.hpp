// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::application
{

// What a catalog SAYS, independent of how the file says it.
//
// The rig compares catalogs by sha256 over their bytes, which cannot tell
// "SQLite folded the write-ahead log into the database" from "the library
// changed" -- a checkpoint rewrites the file, so both look identical to a
// byte comparison. Windows round 7's F4-undo failed on exactly that, and
// the round could only report the difference, never explain it.
//
// Telling them apart means reading the file as a catalog. exportLibrary.db
// is SQLCipher-encrypted, so neither sha256sum nor sqlite3 can do it;
// Seabass can, because it already has the readers.
//
// The digest covers what a DJ would notice changing: the tracks, their
// authored metadata, and their cues. It deliberately leaves out anything
// derived or best-effort -- resolved absolute paths, file sizes, artwork
// paths, whether a duration was estimated -- because those depend on the
// machine doing the reading and would make two honest reads of one stick
// disagree.
//
// Positions are rounded to the millisecond. Cue positions round-trip
// through each format's own units (Engine stores samples), so a write
// followed by its undo can return a value that differs in the tenth
// decimal and means the same cue. That tolerance is the project's
// existing one, not a new judgement: "the same cue is the same slot
// within a millisecond" already decides this in the Engine cue tests.
// It is a tolerance, and it is the one thing here that could hide a
// genuine difference, so it is stated rather than buried.
std::string catalogDigest(const std::vector<domain::Track> &tracks);

// The same content, line by line, for a caller that needs to SHOW which
// track differs rather than only that something did. The digest is the
// sha256 of exactly these lines joined by "\n".
std::vector<std::string> catalogDigestLines(const std::vector<domain::Track> &tracks);

}  // namespace seabass::application
