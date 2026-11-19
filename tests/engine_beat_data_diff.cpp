// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// For the copy-beatgrids CLI tests: which Engine rows' beatData differ
// between two m.db files, byte for byte, and how many entries the beat
// grid ledger of a stick holds. Prints
//
//   rows: <rows in the first>/<rows in the second>
//   beat data changed: <n>
//   changed: <trackId>        (one line per row, by id)
//   ledger entries: <n>       (with a stick root as the third argument)
//
// and leaves the matching to the test's PASS_REGULAR_EXPRESSION. Both
// databases are opened read-only and immutable, so neither file, nor a
// journal beside it, is touched: the first is the fixture itself.

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace
{

using BeatData = std::optional<std::vector<unsigned char>>;

bool readBeatData(const std::string &file, std::map<std::int64_t, BeatData> &out)
{
    sqlite3 *handle = nullptr;
    const std::string uri = "file:" + file + "?immutable=1";
    if (sqlite3_open_v2(uri.c_str(), &handle, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        std::cerr << "cannot open " << file << ": " << sqlite3_errmsg(handle) << "\n";
        sqlite3_close(handle);
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(handle, "SELECT trackId, beatData FROM PerformanceData", -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "cannot read " << file << ": " << sqlite3_errmsg(handle) << "\n";
        sqlite3_close(handle);
        return false;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::int64_t id = sqlite3_column_int64(stmt, 0);
        if (sqlite3_column_type(stmt, 1) == SQLITE_NULL) {
            out[id] = std::nullopt;
        } else {
            const auto *bytes = static_cast<const unsigned char *>(sqlite3_column_blob(stmt, 1));
            out[id] = std::vector<unsigned char>(bytes, bytes + sqlite3_column_bytes(stmt, 1));
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: engine_beat_data_diff BEFORE_M_DB AFTER_M_DB [STICK_ROOT]\n";
        return 2;
    }
    std::map<std::int64_t, BeatData> before;
    std::map<std::int64_t, BeatData> after;
    if (!readBeatData(argv[1], before) || !readBeatData(argv[2], after)) {
        return 2;
    }
    std::vector<std::int64_t> changed;
    for (const auto &[id, data] : after) {
        const auto was = before.find(id);
        if (was == before.end() || was->second != data) {
            changed.push_back(id);
        }
    }
    for (const auto &[id, data] : before) {
        if (!after.contains(id)) {
            changed.push_back(id);
        }
    }
    std::cout << "rows: " << before.size() << "/" << after.size() << "\n";
    std::cout << "beat data changed: " << changed.size() << "\n";
    for (const auto id : changed) {
        std::cout << "changed: " << id << "\n";
    }
    if (argc == 4) {
        std::string error;
        const auto ledger =
            seabass::infrastructure::local::readBeatGridLedger(seabass::pathFromUtf8(argv[3]), &error);
        if (!error.empty()) {
            std::cout << "ledger error: " << error << "\n";
        }
        std::cout << "ledger entries: " << ledger.size() << "\n";
    }
    return 0;
}
