// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace seabass::domain
{

// A musical key's position on the Camelot wheel (the standard,
// vendor-neutral DJ harmonic-mixing convention -- see Theme.qml's own
// doc comment on colorForKey() for the fuller explanation this mirrors).
struct CamelotKey
{
    int number = 0;  // 1..12
    bool isMinor = false;

    bool operator==(const CamelotKey &other) const { return number == other.number && isMinor == other.isMinor; }

    // Parses a key string like "Dm", "F#m", "Bb", "C#" -- or Camelot
    // notation itself, e.g. "10A", "3B" (some catalogs store the key
    // field this way already) -- into a wheel position. Accepts Unicode
    // "♯"/"♭" (as libdjinterop's own musical_key operator<< emits, and
    // real rekordbox/Engine data both use) normalized to ASCII first.
    // Mirrors Theme.qml's own parseCamelotKey() (kept in sync by hand --
    // QML can't share this code) and returns nullopt for anything
    // unrecognized rather than guessing wrong.
    static std::optional<CamelotKey> parse(const std::string &key);
};

// How two keys relate on the Camelot wheel -- the standard DJ harmonic-
// mixing vocabulary, ordered loosely by how safe a transition each one
// represents. The enum's own declaration order doubles as a "how close"
// rank (lower is closer) wherever a caller wants to sort by it, e.g.
// static_cast<int>(relation).
enum class KeyRelation
{
    Unknown,        // either key didn't parse
    Same,           // identical wheel position and mode
    Relative,       // same number, other mode (relative major/minor)
    AdjacentUp,     // one step clockwise around the wheel, same mode -- "Energy Boost"
    AdjacentDown,   // one step counter-clockwise around the wheel, same mode -- "Energy Drop"
    EnergyMix,      // one step around the wheel, other mode -- the "energy mix" move
    Unrelated,      // anything else
};

// Classifies the relation FROM keyA TO keyB. Symmetric for every value
// except AdjacentUp/AdjacentDown -- Same/Relative/EnergyMix/Unrelated/
// Unknown classify (a, b) and (b, a) identically, but the one-step-same-
// mode case is inherently directional (moving from A to B is a lift in
// energy one way around the wheel, a drop the other), so swapping the
// arguments swaps AdjacentUp and AdjacentDown. Pass keyA as whichever key
// comes first in the transition being described (e.g. "the track
// currently playing" -> "the candidate to mix into").
KeyRelation classifyKeyRelation(const std::string &keyA, const std::string &keyB);

// Short, human-readable label for a relation -- e.g. for a tooltip or an
// inline note next to a candidate track ("Same key", "Relative major/
// minor", ...). Empty for Unknown: there's nothing honest to say about a
// key that didn't parse, same "don't fabricate it" stance Theme.qml's
// own key handling already takes.
std::string keyRelationLabel(KeyRelation relation);

// True if `relation` matches any of the tiers named in `keyModes` -- an
// additive (union) filter, not a cumulative range: each name maps to
// exactly one relation ("match" -> Same, "relative" -> Relative,
// "harmonic" -> Adjacent, the classic harmonic-mixing move, "energymix"
// -> EnergyMix), and a candidate matches if its relation is any of the
// tiers picked. An empty `keyModes` means no key filtering at all
// (matches everything, including Unknown/unparseable keys) -- the
// filter row's "All" state. The single source of truth for what each
// tier name means: every caller that filters by key tier --
// ScanController::findCompatibleTracks(), the Matching panel's
// own key-tier row -- calls this rather than re-deriving the mapping
// itself. "harmonic" matches either direction of the adjacent tier
// (AdjacentUp or AdjacentDown) -- the tier filter is about harmonic
// closeness, not which way the energy moves.
bool keyRelationMatchesAnyMode(KeyRelation relation, const std::vector<std::string> &keyModes);

}  // namespace seabass::domain
