// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/fuzzy_matcher.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace seabass::domain
{

namespace
{

std::string toLower(const std::string &s)
{
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Classic Levenshtein edit distance (insert/delete/substitute), O(n*m).
size_t levenshteinDistance(const std::string &a, const std::string &b)
{
    std::vector<size_t> previous(b.size() + 1);
    std::vector<size_t> current(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) {
        previous[j] = j;
    }

    for (size_t i = 1; i <= a.size(); ++i) {
        current[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
        }
        std::swap(previous, current);
    }
    return previous[b.size()];
}

}  // namespace

double FuzzyMatcher::score(const std::string &query, const std::string &candidate)
{
    if (query.empty() || candidate.empty()) {
        return 0.0;
    }

    std::string q = toLower(query);
    std::string c = toLower(candidate);

    if (c.find(q) != std::string::npos) {
        return 1.0;
    }

    size_t distance = levenshteinDistance(q, c);
    size_t longest = std::max(q.size(), c.size());
    return 1.0 - static_cast<double>(distance) / static_cast<double>(longest);
}

}  // namespace seabass::domain
