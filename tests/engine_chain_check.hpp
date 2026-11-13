// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// Whether an Engine m.db's playlists still walk, the way a player walks
// them: the C++ form of the walk.py used on the #73/#74 hardware rounds.
//
// Engine keeps order as linked lists (engine_playlists.hpp, "How the
// chains stay walkable"): PlaylistEntity.nextEntityId within one list and
// Playlist.nextListId among the children of one parent, 0 ending each.
// checkEngineChains() reads the database read-only and reports, rather
// than asserts, every way that breaks, so a test can print all of them:
//
// - PRAGMA integrity_check is not "ok";
// - an entry whose list, or whose track, does not exist;
// - an entry pointing at an entry that is not in its own list (into
//   another list's chain, or at nothing);
// - two entries pointing at the same entry;
// - a list without exactly one head (an entry nothing points at) and one
//   tail (nextEntityId 0);
// - a walk from the head that loops, or that does not reach every entry
//   of the list (the count does not match);
// - the same for each parent's children through nextListId, and a
//   playlist whose parent does not exist.
//
// A list or parent with no rows is whole. Read-only: the file is opened
// with SQLITE_OPEN_READONLY, so a fixture is never touched (and a test
// works on a copy anyway).

#include <sqlite3.h>

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace seabass::testing
{

struct EngineChainReport
{
    // Empty when every chain walks and integrity_check is ok.
    std::vector<std::string> problems;
    // Each list's track ids in chain order, for the lists that walked.
    std::map<std::int64_t, std::vector<std::int64_t>> entryTracks;
    // Each parent's child playlist ids in nextListId order (0: the
    // roots), for the parents whose children walked.
    std::map<std::int64_t, std::vector<std::int64_t>> children;

    bool whole() const { return problems.empty(); }
    std::string text() const
    {
        std::string out;
        for (const auto &p : problems) {
            out += "  " + p + "\n";
        }
        return out;
    }
};

namespace detail
{

inline std::vector<std::vector<std::int64_t>> chainIntRows(sqlite3 *db, const char *sql, std::string &error)
{
    std::vector<std::vector<std::int64_t>> out;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = std::string(sql) + ": " + sqlite3_errmsg(db);
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::vector<std::int64_t> row;
        for (int c = 0; c < sqlite3_column_count(stmt); ++c) {
            row.push_back(sqlite3_column_int64(stmt, c));
        }
        out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return out;
}

// One linked list: `nextOf` maps each row to the row after it (0 ends).
// Appends what is wrong with it to `problems`, prefixed by `what`, and
// returns the rows in order when it walks whole.
inline std::vector<std::int64_t> walkChain(const std::map<std::int64_t, std::int64_t> &nextOf,
                                           const std::string &what, std::vector<std::string> &problems)
{
    const std::size_t before = problems.size();
    std::map<std::int64_t, int> pointedAt;
    std::vector<std::int64_t> tails;
    for (const auto &[id, next] : nextOf) {
        if (next == 0) {
            tails.push_back(id);
            continue;
        }
        if (!nextOf.count(next)) {
            problems.push_back(what + ": row " + std::to_string(id) + " points at " + std::to_string(next)
                               + ", which is not in this chain");
        }
        if (++pointedAt[next] == 2) {
            problems.push_back(what + ": two rows point at " + std::to_string(next));
        }
    }
    std::vector<std::int64_t> heads;
    for (const auto &[id, next] : nextOf) {
        if (!pointedAt.count(id)) {
            heads.push_back(id);
        }
    }
    if (heads.size() != 1 || tails.size() != 1) {
        problems.push_back(what + ": " + std::to_string(heads.size()) + " head(s) and " + std::to_string(tails.size())
                           + " tail(s), not one of each");
    }
    std::vector<std::int64_t> order;
    if (!heads.empty()) {
        std::set<std::int64_t> seen;
        for (std::int64_t at = heads.front(); at != 0;) {
            const auto it = nextOf.find(at);
            if (it == nextOf.end()) {
                break;  // said above: points outside the chain
            }
            if (!seen.insert(at).second) {
                problems.push_back(what + ": the walk loops at " + std::to_string(at));
                break;
            }
            order.push_back(at);
            at = it->second;
        }
    }
    if (order.size() != nextOf.size()) {
        problems.push_back(what + ": the walk from the head reaches " + std::to_string(order.size()) + " of "
                           + std::to_string(nextOf.size()) + " rows");
    }
    return problems.size() == before ? order : std::vector<std::int64_t>{};
}

}  // namespace detail

inline EngineChainReport checkEngineChains(const std::string &dbFile)
{
    EngineChainReport report;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(dbFile.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        report.problems.push_back("cannot open " + dbFile + ": " + (db ? sqlite3_errmsg(db) : "no handle"));
        sqlite3_close(db);
        return report;
    }
    std::string error;

    {
        sqlite3_stmt *stmt = nullptr;
        std::vector<std::string> lines;
        if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                lines.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)));
            }
        }
        sqlite3_finalize(stmt);
        if (lines != std::vector<std::string>{"ok"}) {
            for (const auto &line : lines) {
                report.problems.push_back("integrity_check: " + line);
            }
            if (lines.empty()) {
                report.problems.push_back("integrity_check gave no answer");
            }
        }
    }

    std::set<std::int64_t> tracks;
    for (const auto &r : detail::chainIntRows(db, "SELECT id FROM Track;", error)) {
        tracks.insert(r[0]);
    }
    std::map<std::int64_t, std::map<std::int64_t, std::int64_t>> playlistsByParent;
    std::set<std::int64_t> playlists;
    for (const auto &r : detail::chainIntRows(db, "SELECT id, parentListId, nextListId FROM Playlist;", error)) {
        playlists.insert(r[0]);
        playlistsByParent[r[1]][r[0]] = r[2];
    }
    std::map<std::int64_t, std::map<std::int64_t, std::int64_t>> entriesByList;
    std::map<std::int64_t, std::int64_t> trackOf;
    for (const auto &r :
         detail::chainIntRows(db, "SELECT id, listId, trackId, nextEntityId FROM PlaylistEntity;", error)) {
        entriesByList[r[1]][r[0]] = r[3];
        trackOf[r[0]] = r[2];
        if (!playlists.count(r[1])) {
            report.problems.push_back("entry " + std::to_string(r[0]) + " is in list " + std::to_string(r[1])
                                      + ", which does not exist");
        }
        if (!tracks.count(r[2])) {
            report.problems.push_back("entry " + std::to_string(r[0]) + " names track " + std::to_string(r[2])
                                      + ", which does not exist");
        }
    }
    sqlite3_close(db);
    if (!error.empty()) {
        report.problems.push_back(error);
        return report;
    }

    for (const auto &[parent, chain] : playlistsByParent) {
        if (parent != 0 && !playlists.count(parent)) {
            report.problems.push_back("playlists under " + std::to_string(parent) + ", which does not exist");
        }
        const auto order = detail::walkChain(chain, "children of " + std::to_string(parent), report.problems);
        if (order.size() == chain.size()) {
            report.children[parent] = order;
        }
    }
    for (const auto &[list, chain] : entriesByList) {
        const auto order = detail::walkChain(chain, "list " + std::to_string(list), report.problems);
        if (order.size() == chain.size()) {
            auto &out = report.entryTracks[list];
            for (const std::int64_t entry : order) {
                out.push_back(trackOf.at(entry));
            }
        }
    }
    return report;
}

}  // namespace seabass::testing
