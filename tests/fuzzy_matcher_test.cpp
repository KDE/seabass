// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/fuzzy_matcher.hpp"

using namespace seabass::domain;

int main()
{
    // Exact match.
    assert(FuzzyMatcher::score("Concorde", "Concorde") == 1.0);
    std::cout << "case 1 (exact) OK\n";

    // Case-insensitive substring match.
    assert(FuzzyMatcher::score("concorde", "Concorde") == 1.0);
    assert(FuzzyMatcher::score("Concorde", "15_Miss Monique-Concorde.mp3") == 1.0);
    std::cout << "case 2 (case-insensitive substring) OK\n";

    // Minor typo still scores highly.
    double typoScore = FuzzyMatcher::score("Concordi", "Concorde");
    assert(typoScore > 0.7 && typoScore < 1.0);
    std::cout << "case 3 (typo tolerant, score=" << typoScore << ") OK\n";

    // Unrelated strings score low.
    double unrelated = FuzzyMatcher::score("Concorde", "New Morning (Original Mix)");
    assert(unrelated < 0.3);
    std::cout << "case 4 (unrelated, score=" << unrelated << ") OK\n";

    // Empty query/candidate never matches.
    assert(FuzzyMatcher::score("", "Concorde") == 0.0);
    assert(FuzzyMatcher::score("Concorde", "") == 0.0);
    std::cout << "case 5 (empty strings) OK\n";

    std::cout << "all cases passed\n";
    return 0;
}
