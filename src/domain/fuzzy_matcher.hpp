// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::domain
{

// Simple, dependency-free fuzzy text matching for looking up a track by
// (partial, possibly misspelled) name. Case-insensitive.
class FuzzyMatcher
{
public:
    // Returns a similarity score in [0, 1]: 1.0 for an exact or substring
    // match, decreasing with edit distance otherwise (so minor typos still
    // match, but unrelated strings score near zero). 0.0 if either string
    // is empty.
    static double score(const std::string &query, const std::string &candidate);
};

}  // namespace seabass::domain
