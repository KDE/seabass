// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace seabass::infrastructure::engine
{

// Whether an Engine player will offer to re-import the rekordbox library
// on this stick, and the one number that decides it.
//
// Engine records, in Information.lastRekordBoxLibraryImportReadCounter,
// the rekordbox library's own sequence number as it stood when Engine
// last imported it. export.pdb carries that sequence in its header and
// moves it every time the library is written. On insert the player
// compares the two, and when they differ it asks:
//
//   "Would you like to update the Rekordbox library from 'A3 (USB 1)'?
//    The device will be unavailable and the update could take some time.
//    Existing playlist and track metadata will be overwritten."
//
// That last sentence is why this is worth reporting rather than leaving
// to chance. Accepting rewrites the Engine side from the rekordbox one:
// it is what puts "image://fileart//<path on the importing computer>"
// where cover art used to be, and it would take out every cue, playlist
// and cover this app has repaired on the Engine side.
//
// Measured on a Prime 4 and a Prime Go+ (2026-09-18): with the two
// numbers level, neither player asks; with them apart, both do.
struct RekordboxImportState
{
    bool hasEngineLibrary = false;
    bool hasRekordboxLibrary = false;
    std::uint64_t engineCounter = 0;   // what Engine last imported
    std::uint64_t librarySequence = 0; // what the rekordbox library says now
    std::string error;

    // The player will ask on the next insert.
    bool playerWillOfferImport() const
    {
        return hasEngineLibrary && hasRekordboxLibrary && error.empty() && engineCounter != librarySequence;
    }
};

RekordboxImportState readRekordboxImportState(const std::string &engineLibraryPath,
                                               const std::string &pioneerPath);

// Writes the library's current sequence into Engine's Information row:
// "this is imported, do not ask". Returns false with `error` set when the
// database could not be written.
bool markRekordboxLibraryImported(const std::string &engineLibraryPath, std::uint64_t librarySequence,
                                  std::string *error = nullptr,
                                  const std::function<void(const std::string &)> &beforeWrite = {},
                                  const std::string &databaseFileOverride = {});

}  // namespace seabass::infrastructure::engine
