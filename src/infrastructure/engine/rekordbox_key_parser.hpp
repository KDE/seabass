// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

#include <djinterop/musical_key.hpp>

namespace seabass::infrastructure::engine
{

// Parses a rekordbox-style key string (e.g. "Fm", "F#m", "Gbm", "C",
// "A♭") into libdjinterop's musical_key enum, by pitch class + mode
// rather than by exact spelling, so either enharmonic spelling ("F#m" or
// "Gbm") maps to the same key. Handles both ASCII ('#'/'b') and Unicode
// ('♯'/'♭') accidentals. Returns nullopt for anything it doesn't
// recognize (including Camelot notation, e.g. "8A" -- not supported in
// this first version) rather than guessing wrong.
std::optional<djinterop::musical_key> parseRekordboxKey(const std::string &key);

// Parses Camelot notation ("8A", "11B", "05A"; 1 to 12, A for minor, B
// for major) into the same enum, and nothing else: a note name is
// parseRekordboxKey()'s. Kept apart so callers that have always seen
// nullopt for "8A" keep seeing it; a caller ready for both tries one,
// then the other. The wheel fixes one key per position (8B is C major,
// 8A its relative minor, A minor), so there is nothing to guess.
std::optional<djinterop::musical_key> parseCamelotKey(const std::string &key);

}  // namespace seabass::infrastructure::engine
