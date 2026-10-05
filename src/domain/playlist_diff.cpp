// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playlist_diff.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace seabass::domain
{

namespace
{

// Hirschberg's longest common subsequence: halve A, find where an
// optimal path crosses the middle row from one forward and one backward
// pass of the length table, one row each, and recurse on the two
// corners. O(n*m) time like the full table, O(n+m) memory instead of
// (n+1)*(m+1) cells: two 20000-entry playlists would need 1.6 GB.
class Hirschberg
{
public:
    Hirschberg(const std::vector<int> &a, const std::vector<int> &b, const std::function<bool()> &stop,
               std::vector<std::pair<int, int>> &out)
        : m_a(a), m_b(b), m_stop(stop), m_out(out),
          m_forward(b.size() + 1, 0), m_backward(b.size() + 1, 0)
    {
    }

    void run() { solve(0, static_cast<int>(m_a.size()), 0, static_cast<int>(m_b.size())); }
    bool stopped() const { return m_stopped; }
    std::size_t cells() const { return m_forward.size() + m_backward.size(); }

private:
    // Polled every few hundred rows, so a pass over 20000 columns
    // notices within a few milliseconds.
    bool shouldStop()
    {
        if (!m_stopped && m_stop && ++m_polls % 256 == 0) {
            m_stopped = m_stop();
        }
        return m_stopped;
    }

    // row[k] = LCS(a[a0..a1), b[b0..b0+k)), k = 0..b1-b0.
    void forward(int a0, int a1, int b0, int b1)
    {
        const int len = b1 - b0;
        std::uint32_t *row = m_forward.data();
        std::fill(row, row + len + 1, 0u);
        for (int i = a0; i < a1 && !shouldStop(); ++i) {
            const int x = m_a[static_cast<std::size_t>(i)];
            std::uint32_t diagonal = 0;
            for (int k = 1; k <= len; ++k) {
                const std::uint32_t above = row[k];
                row[k] = m_b[static_cast<std::size_t>(b0 + k - 1)] == x ? diagonal + 1 : std::max(above, row[k - 1]);
                diagonal = above;
            }
        }
    }

    // row[k] = LCS(a[a0..a1), b[b0+k..b1)), k = 0..b1-b0.
    void backward(int a0, int a1, int b0, int b1)
    {
        const int len = b1 - b0;
        std::uint32_t *row = m_backward.data();
        std::fill(row, row + len + 1, 0u);
        for (int i = a1 - 1; i >= a0 && !shouldStop(); --i) {
            const int x = m_a[static_cast<std::size_t>(i)];
            std::uint32_t diagonal = 0;
            for (int k = len - 1; k >= 0; --k) {
                const std::uint32_t below = row[k];
                row[k] = m_b[static_cast<std::size_t>(b0 + k)] == x ? diagonal + 1 : std::max(below, row[k + 1]);
                diagonal = below;
            }
        }
    }

    void solve(int a0, int a1, int b0, int b1)
    {
        if (shouldStop()) {
            return;
        }
        // A shared head and tail are part of some longest subsequence:
        // taken as they are, which also makes near copies cheap.
        while (a0 < a1 && b0 < b1 && m_a[static_cast<std::size_t>(a0)] == m_b[static_cast<std::size_t>(b0)]) {
            m_out.emplace_back(a0++, b0++);
        }
        int tail = 0;
        while (a1 - tail > a0 && b1 - tail > b0
               && m_a[static_cast<std::size_t>(a1 - 1 - tail)] == m_b[static_cast<std::size_t>(b1 - 1 - tail)]) {
            ++tail;
        }
        a1 -= tail;
        b1 -= tail;
        if (a0 < a1 && b0 < b1) {
            if (a1 - a0 == 1) {
                const int x = m_a[static_cast<std::size_t>(a0)];
                for (int j = b0; j < b1; ++j) {
                    if (m_b[static_cast<std::size_t>(j)] == x) {
                        m_out.emplace_back(a0, j);
                        break;
                    }
                }
            } else if (b1 - b0 == 1) {
                const int y = m_b[static_cast<std::size_t>(b0)];
                for (int i = a0; i < a1; ++i) {
                    if (m_a[static_cast<std::size_t>(i)] == y) {
                        m_out.emplace_back(i, b0);
                        break;
                    }
                }
            } else {
                const int mid = a0 + (a1 - a0) / 2;
                forward(a0, mid, b0, b1);
                backward(mid, a1, b0, b1);
                int split = 0;
                std::uint32_t best = 0;
                for (int k = 0; k <= b1 - b0; ++k) {
                    const std::uint32_t total = m_forward[static_cast<std::size_t>(k)] + m_backward[static_cast<std::size_t>(k)];
                    if (total > best || k == 0) {
                        best = total;
                        split = k;
                    }
                }
                solve(a0, mid, b0, b0 + split);
                solve(mid, a1, b0 + split, b1);
            }
        }
        for (int t = 0; t < tail; ++t) {
            m_out.emplace_back(a1 + t, b1 + t);
        }
    }

    const std::vector<int> &m_a;
    const std::vector<int> &m_b;
    const std::function<bool()> &m_stop;
    std::vector<std::pair<int, int>> &m_out;
    std::vector<std::uint32_t> m_forward;
    std::vector<std::uint32_t> m_backward;
    unsigned m_polls = 0;
    bool m_stopped = false;
};

// The same string, the same small int, across both lists.
struct Interned
{
    std::vector<int> a;
    std::vector<int> b;
    int distinct = 0;  // ids run 0 .. distinct - 1
};

Interned intern(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    std::unordered_map<std::string, int> ids;
    auto idOf = [&ids](const std::string &s) {
        auto [it, inserted] = ids.emplace(s, static_cast<int>(ids.size()));
        return it->second;
    };
    Interned out;
    out.a.reserve(a.size());
    out.b.reserve(b.size());
    for (const auto &s : a) {
        out.a.push_back(idOf(s));
    }
    for (const auto &s : b) {
        out.b.push_back(idOf(s));
    }
    out.distinct = static_cast<int>(ids.size());
    return out;
}

}  // namespace

namespace detail
{

std::vector<std::pair<int, int>> longestCommonSubsequence(const std::vector<int> &a, const std::vector<int> &b,
                                                          const std::function<bool()> &stop, std::size_t *tableCells)
{
    // An entry whose track the other list lacks can never be matched:
    // left out, two playlists with little in common cost next to nothing.
    int ids = 0;
    for (int x : a) {
        ids = std::max(ids, x + 1);
    }
    for (int y : b) {
        ids = std::max(ids, y + 1);
    }
    std::vector<char> inA(static_cast<std::size_t>(ids), 0);
    std::vector<char> inB(static_cast<std::size_t>(ids), 0);
    for (int x : a) {
        inA[static_cast<std::size_t>(x)] = 1;
    }
    for (int y : b) {
        inB[static_cast<std::size_t>(y)] = 1;
    }
    std::vector<int> keptA;
    std::vector<int> keptB;
    std::vector<int> indexA;
    std::vector<int> indexB;
    for (int i = 0; i < static_cast<int>(a.size()); ++i) {
        if (inB[static_cast<std::size_t>(a[static_cast<std::size_t>(i)])]) {
            keptA.push_back(a[static_cast<std::size_t>(i)]);
            indexA.push_back(i);
        }
    }
    for (int j = 0; j < static_cast<int>(b.size()); ++j) {
        if (inA[static_cast<std::size_t>(b[static_cast<std::size_t>(j)])]) {
            keptB.push_back(b[static_cast<std::size_t>(j)]);
            indexB.push_back(j);
        }
    }

    std::vector<std::pair<int, int>> pairs;
    Hirschberg lcs(keptA, keptB, stop, pairs);
    lcs.run();
    if (tableCells) {
        *tableCells = lcs.cells();
    }
    for (auto &[i, j] : pairs) {
        i = indexA[static_cast<std::size_t>(i)];
        j = indexB[static_cast<std::size_t>(j)];
    }
    return pairs;
}

}  // namespace detail

PlaylistDiff diffPlaylists(const std::vector<std::string> &a, const std::vector<std::string> &b,
                           const std::function<bool()> &stop)
{
    const Interned ids = intern(a, b);
    const int n = static_cast<int>(ids.a.size());
    const int m = static_cast<int>(ids.b.size());

    PlaylistDiff diff;
    auto pairs = detail::longestCommonSubsequence(ids.a, ids.b, stop);
    if (stop && stop()) {
        return diff;
    }

    // The copies of each track the common subsequence left unpinned, in
    // list order. The k-th such copy in A and the k-th in B are one entry
    // moved, each the other's partner. A copy beyond what the other side
    // has unpinned is an extra copy that side alone lists: never a move,
    // since nothing on the other side moved to stand for it.
    std::vector<char> pinnedA(static_cast<std::size_t>(n), 0);
    std::vector<char> pinnedB(static_cast<std::size_t>(m), 0);
    for (const auto &[ai, bj] : pairs) {
        pinnedA[static_cast<std::size_t>(ai)] = 1;
        pinnedB[static_cast<std::size_t>(bj)] = 1;
    }
    const auto tracks = static_cast<std::size_t>(ids.distinct);
    std::vector<int> copiesInA(tracks, 0);
    std::vector<int> copiesInB(tracks, 0);
    std::vector<std::vector<int>> looseA(tracks);
    std::vector<std::vector<int>> looseB(tracks);
    for (int i = 0; i < n; ++i) {
        const auto id = static_cast<std::size_t>(ids.a[static_cast<std::size_t>(i)]);
        ++copiesInA[id];
        if (!pinnedA[static_cast<std::size_t>(i)]) {
            looseA[id].push_back(i);
        }
    }
    for (int j = 0; j < m; ++j) {
        const auto id = static_cast<std::size_t>(ids.b[static_cast<std::size_t>(j)]);
        ++copiesInB[id];
        if (!pinnedB[static_cast<std::size_t>(j)]) {
            looseB[id].push_back(j);
        }
    }
    std::vector<int> partnerInB(static_cast<std::size_t>(n), -1);
    std::vector<int> partnerInA(static_cast<std::size_t>(m), -1);
    for (std::size_t id = 0; id < tracks; ++id) {
        const std::size_t moves = std::min(looseA[id].size(), looseB[id].size());
        for (std::size_t k = 0; k < moves; ++k) {
            partnerInB[static_cast<std::size_t>(looseA[id][k])] = looseB[id][k];
            partnerInA[static_cast<std::size_t>(looseB[id][k])] = looseA[id][k];
        }
    }

    // A closing sentinel so the tail after the last agreed row is laid
    // out by the same loop.
    pairs.emplace_back(n, m);

    int i = 0;
    int j = 0;
    for (const auto &[ai, bj] : pairs) {
        std::vector<DiffEntry> left;
        std::vector<DiffEntry> right;
        for (; i < ai; ++i) {
            DiffEntry entry;
            entry.index = i;
            const int partner = partnerInB[static_cast<std::size_t>(i)];
            if (partner >= 0) {
                entry.kind = DiffEntryKind::Moved;
                entry.partner = partner;
                ++diff.shared;
                ++diff.moved;
            } else {
                entry.kind = DiffEntryKind::Only;
                ++diff.onlyA;
                if (copiesInB[static_cast<std::size_t>(ids.a[static_cast<std::size_t>(i)])] > 0) {
                    ++diff.extraA;
                }
            }
            left.push_back(entry);
        }
        for (; j < bj; ++j) {
            DiffEntry entry;
            entry.index = j;
            const int partner = partnerInA[static_cast<std::size_t>(j)];
            if (partner >= 0) {
                entry.kind = DiffEntryKind::Moved;
                entry.partner = partner;
            } else {
                entry.kind = DiffEntryKind::Only;
                ++diff.onlyB;
                if (copiesInA[static_cast<std::size_t>(ids.b[static_cast<std::size_t>(j)])] > 0) {
                    ++diff.extraB;
                }
            }
            right.push_back(entry);
        }
        // Between two agreed rows the two sides are laid out top-aligned,
        // one row per line of the longer side.
        const std::size_t lines = std::max(left.size(), right.size());
        for (std::size_t k = 0; k < lines; ++k) {
            DiffRow row;
            if (k < left.size()) {
                row.a = left[k];
            }
            if (k < right.size()) {
                row.b = right[k];
            }
            diff.rows.push_back(row);
        }
        if (ai < n) {
            DiffRow row;
            row.a.kind = DiffEntryKind::Same;
            row.a.index = ai;
            row.b.kind = DiffEntryKind::Same;
            row.b.index = bj;
            diff.rows.push_back(row);
            ++diff.shared;
        }
        i = ai + 1;
        j = bj + 1;
    }
    return diff;
}

PlaylistOverlap overlapOf(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    const std::unordered_set<std::string> setA(a.begin(), a.end());
    const std::unordered_set<std::string> setB(b.begin(), b.end());
    PlaylistOverlap out;
    for (const auto &s : setA) {
        if (setB.count(s)) {
            ++out.shared;
        } else {
            ++out.onlyA;
        }
    }
    out.onlyB = static_cast<int>(setB.size()) - out.shared;
    const int either = out.shared + out.onlyA + out.onlyB;
    out.jaccard = either > 0 ? static_cast<double>(out.shared) / either : 0.0;
    if (out.shared == 0) {
        out.relation = PlaylistRelation::Disjoint;
    } else if (out.onlyA == 0 && out.onlyB == 0) {
        out.relation = PlaylistRelation::Identical;
    } else if (out.onlyA == 0) {
        out.relation = PlaylistRelation::Superset;
    } else if (out.onlyB == 0) {
        out.relation = PlaylistRelation::Subset;
    } else {
        out.relation = PlaylistRelation::Overlap;
    }
    return out;
}

}  // namespace seabass::domain
