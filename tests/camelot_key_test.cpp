// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/camelot_key.hpp"

using namespace seabass::domain;

int main()
{
    // parse(): traditional notation, both accidental spellings.
    {
        auto fMinor = CamelotKey::parse("Fm");
        assert(fMinor.has_value());
        assert(fMinor->number == 4);
        assert(fMinor->isMinor);

        auto fSharpMinor = CamelotKey::parse("F#m");
        assert(fSharpMinor.has_value());
        assert(fSharpMinor->number == 11);
        assert(fSharpMinor->isMinor);

        // Unicode accidentals, as libdjinterop's musical_key operator<<
        // actually emits for real Engine data.
        auto fSharpMinorUnicode = CamelotKey::parse("F♯m");
        assert(fSharpMinorUnicode == fSharpMinor);

        auto cMajor = CamelotKey::parse("C");
        assert(cMajor.has_value());
        assert(cMajor->number == 8);
        assert(!cMajor->isMinor);

        std::cout << "case 1 (parse traditional notation) OK\n";
    }

    // parse(): Camelot notation stored directly as the key string.
    {
        auto camelot = CamelotKey::parse("10A");
        assert(camelot.has_value());
        assert(camelot->number == 10);
        assert(camelot->isMinor);

        auto camelotMajor = CamelotKey::parse("3B");
        assert(camelotMajor.has_value());
        assert(camelotMajor->number == 3);
        assert(!camelotMajor->isMinor);

        std::cout << "case 2 (parse Camelot notation) OK\n";
    }

    // parse(): unrecognized/empty input never guesses.
    {
        assert(!CamelotKey::parse("").has_value());
        assert(!CamelotKey::parse("nonsense").has_value());
        assert(!CamelotKey::parse("13A").has_value());  // out of range
        std::cout << "case 3 (parse rejects unrecognized keys) OK\n";
    }

    // classifyKeyRelation(): the five related tiers plus unrelated/unknown.
    {
        assert(classifyKeyRelation("Am", "Am") == KeyRelation::Same);
        assert(classifyKeyRelation("Am", "C") == KeyRelation::Relative);   // both Camelot 8
        assert(classifyKeyRelation("Am", "Em") == KeyRelation::AdjacentUp);    // 8A -> 9A
        assert(classifyKeyRelation("Am", "Dm") == KeyRelation::AdjacentDown);  // 8A -> 7A
        assert(classifyKeyRelation("Am", "F") == KeyRelation::EnergyMix);  // 8A -> 7B
        assert(classifyKeyRelation("Am", "Bm") == KeyRelation::Unrelated);  // 8A -> 10A, 2 steps
        assert(classifyKeyRelation("Am", "nonsense") == KeyRelation::Unknown);
        assert(classifyKeyRelation("Am", "C") == classifyKeyRelation("C", "Am"));  // symmetric

        // Unlike every other tier, Adjacent is directional: reversing the
        // arguments reverses boost <-> drop (moving 8A -> 9A is a lift,
        // so moving 9A -> 8A back is the drop, not another lift).
        assert(classifyKeyRelation("Em", "Am") == KeyRelation::AdjacentDown);
        std::cout << "case 4 (classifyKeyRelation tiers) OK\n";
    }

    // keyRelationLabel(): non-empty for every real relation, empty only for Unknown.
    {
        assert(keyRelationLabel(KeyRelation::Same) == "Same key");
        assert(!keyRelationLabel(KeyRelation::Relative).empty());
        assert(!keyRelationLabel(KeyRelation::AdjacentUp).empty());
        assert(!keyRelationLabel(KeyRelation::AdjacentDown).empty());
        assert(keyRelationLabel(KeyRelation::AdjacentUp) != keyRelationLabel(KeyRelation::AdjacentDown));
        assert(!keyRelationLabel(KeyRelation::EnergyMix).empty());
        assert(!keyRelationLabel(KeyRelation::Unrelated).empty());
        assert(keyRelationLabel(KeyRelation::Unknown).empty());
        std::cout << "case 5 (keyRelationLabel) OK\n";
    }

    // keyRelationMatchesAnyMode(): the single source of truth for what
    // each tier name maps to -- a plain union, not a cumulative range.
    {
        assert(keyRelationMatchesAnyMode(KeyRelation::Same, {"match"}));
        assert(!keyRelationMatchesAnyMode(KeyRelation::Relative, {"match"}));

        // "relative" matches only Relative, not Same too -- unlike the
        // old cumulative mode names, each tier now stands alone.
        assert(!keyRelationMatchesAnyMode(KeyRelation::Same, {"relative"}));
        assert(keyRelationMatchesAnyMode(KeyRelation::Relative, {"relative"}));

        // "harmonic" matches either direction of the adjacent tier -- the
        // tier filter is about harmonic closeness, not which way the
        // energy moves.
        assert(keyRelationMatchesAnyMode(KeyRelation::AdjacentUp, {"harmonic"}));
        assert(keyRelationMatchesAnyMode(KeyRelation::AdjacentDown, {"harmonic"}));
        assert(!keyRelationMatchesAnyMode(KeyRelation::Same, {"harmonic"}));

        assert(keyRelationMatchesAnyMode(KeyRelation::EnergyMix, {"energymix"}));
        assert(!keyRelationMatchesAnyMode(KeyRelation::Unrelated, {"energymix"}));

        // Any combination of tiers is a union of exactly those relations.
        assert(keyRelationMatchesAnyMode(KeyRelation::Same, {"relative", "match"}));
        assert(keyRelationMatchesAnyMode(KeyRelation::EnergyMix, {"relative", "energymix"}));
        assert(!keyRelationMatchesAnyMode(KeyRelation::AdjacentUp, {"relative", "energymix"}));

        // Empty keyModes means no key filtering at all -- matches
        // anything, Unknown/Unrelated included.
        assert(keyRelationMatchesAnyMode(KeyRelation::Unrelated, {}));
        assert(keyRelationMatchesAnyMode(KeyRelation::Unknown, {}));

        // Unknown never matches a real filter -- an unparseable key
        // isn't a confirmed match to anything.
        assert(!keyRelationMatchesAnyMode(KeyRelation::Unknown, {"match"}));
        assert(!keyRelationMatchesAnyMode(KeyRelation::Unknown, {"harmonic"}));
        assert(!keyRelationMatchesAnyMode(KeyRelation::Unknown, {"energymix"}));

        std::cout << "case 6 (keyRelationMatchesAnyMode) OK\n";
    }

    std::cout << "All camelot_key tests passed.\n";
    return 0;
}
