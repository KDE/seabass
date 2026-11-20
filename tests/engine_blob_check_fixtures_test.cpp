// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// engineBlobDecodable never refuses what a player wrote: every compressed
// PerformanceData blob of the repo's real Engine fixtures (trackData,
// beatData, overviewWaveFormData, quickCues; loops is not compressed) and
// every overview file under Database2/OverviewData passes the check, and
// libdjinterop's own from_blob decodes the same bytes too. A refused
// healthy blob would make Seabass take the row for unreadable and leave it
// alone. The counts are printed, and a column a fixture has analysed rows
// in must have some, so an empty read cannot pass.
//
// Each database is copied into scratch before it is opened.
//
// usage: engine_blob_check_fixtures_test <tests/fixtures>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <djinterop/engine/v3/beat_data_blob.hpp>
#include <djinterop/engine/v3/overview_waveform_data_blob.hpp>
#include <djinterop/engine/v3/quick_cues_blob.hpp>
#include <djinterop/engine/v3/track_data_blob.hpp>
#include <sqlite3.h>

#include "infrastructure/engine/engine_blob_check.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::engine::internal::engineBlobDecodable;
using seabass::pathToUtf8;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

std::vector<std::byte> bytesOf(const std::string &s)
{
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

std::string head(const std::string &s)
{
    std::string out;
    char buf[4];
    for (std::size_t i = 0; i < s.size() && i < 16; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", static_cast<unsigned char>(s[i]));
        out += buf;
    }
    return out;
}

// A four-byte length that is not zero: a zlib stream follows.
bool compressed(const std::string &s)
{
    return s.size() >= 4 && (s[0] != 0 || s[1] != 0 || s[2] != 0 || s[3] != 0);
}

template<typename Blob>
bool libdjinteropDecodes(const std::string &bytes)
{
    try {
        (void)Blob::from_blob(bytesOf(bytes));
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

using Decoder = std::function<bool(const std::string &)>;

struct Column
{
    std::string name;
    Decoder decode;
};

const std::vector<Column> &columns()
{
    static const std::vector<Column> c = {
        {"trackData", libdjinteropDecodes<djinterop::engine::v3::track_data_blob>},
        {"beatData", libdjinteropDecodes<djinterop::engine::v3::beat_data_blob>},
        {"overviewWaveFormData", libdjinteropDecodes<djinterop::engine::v3::overview_waveform_data_blob>},
        {"quickCues", libdjinteropDecodes<djinterop::engine::v3::quick_cues_blob>},
    };
    return c;
}

// One blob through the check and through libdjinterop; true when it counts.
bool run(const std::string &where, const std::string &bytes, const Decoder &decode)
{
    if (!compressed(bytes)) {
        return false;
    }
    const bool accepted = engineBlobDecodable(bytes);
    check(accepted,
          where + ": refused by engineBlobDecodable, " + std::to_string(bytes.size()) + " bytes, first 16: " + head(bytes));
    check(decode(bytes), where + ": libdjinterop does not decode it, " + std::to_string(bytes.size()) + " bytes, first 16: " + head(bytes));
    return true;
}

// `fixture` relative to the fixtures root; `expected` the columns that must
// have compressed blobs (a fixture without analysed rows in one has none).
void checkDatabase(const fs::path &fixtures, const std::string &fixture, const std::vector<std::string> &expected, const fs::path &scratch)
{
    const fs::path source = fixtures / fixture;
    check(fs::exists(source), fixture + ": missing");
    if (!fs::exists(source)) {
        return;
    }
    const fs::path dir = scratch / std::to_string(std::hash<std::string>{}(fixture));
    fs::create_directories(dir);
    const fs::path copy = dir / "m.db";
    fs::copy_file(source, copy, fs::copy_options::overwrite_existing);
    for (const char *suffix : {"-wal", "-journal"}) {
        fs::path side = source;
        side += suffix;
        if (fs::exists(side)) {
            fs::path to = copy;
            to += suffix;
            fs::copy_file(side, to, fs::copy_options::overwrite_existing);
        }
    }

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(pathToUtf8(copy).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        check(false, fixture + ": cannot open " + sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }
    for (const Column &column : columns()) {
        const std::string sql = "SELECT trackId, " + column.name + " FROM PerformanceData WHERE typeof(" + column.name + ") = 'blob'";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            check(false, fixture + ": " + sql + ": " + sqlite3_errmsg(db));
            continue;
        }
        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto id = sqlite3_column_int64(stmt, 0);
            const auto *data = static_cast<const char *>(sqlite3_column_blob(stmt, 1));
            const std::string bytes(data ? data : "", static_cast<std::size_t>(sqlite3_column_bytes(stmt, 1)));
            if (run(fixture + " " + column.name + " row " + std::to_string(id), bytes, column.decode)) {
                ++count;
            }
        }
        sqlite3_finalize(stmt);
        std::cout << fixture << " " << column.name << ": " << count << " compressed blobs\n";
        for (const std::string &name : expected) {
            if (name == column.name) {
                check(count > 0, fixture + " " + column.name + ": no compressed blobs read");
            }
        }
    }
    sqlite3_close(db);
}

// Overview files: the cue writer reads them through the same check.
// 974.rgb holds a complete zlib stream whose overview says 34 points in its first
// count and 1024 in its second: the check passes it (it is safe to hand
// over) and libdjinterop refuses it ("conflicting length fields"), so the
// writer takes it for no overview file. Pinned as that, both ways.
void checkOverviewFiles(const fs::path &fixtures, const std::string &library, int expectAtLeast)
{
    const std::string inconsistent = "974.rgb";
    const fs::path root = fixtures / library / "Database2" / "OverviewData";
    int count = 0;
    if (fs::exists(root)) {
        for (const auto &entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (pathToUtf8(entry.path().filename()) == inconsistent) {
                check(engineBlobDecodable(bytes), inconsistent + ": refused by engineBlobDecodable");
                check(!libdjinteropDecodes<djinterop::engine::v3::overview_waveform_data_blob>(bytes),
                      inconsistent + ": libdjinterop now decodes it; drop the exception");
                continue;
            }
            if (run(pathToUtf8(entry.path()), bytes, libdjinteropDecodes<djinterop::engine::v3::overview_waveform_data_blob>)) {
                ++count;
            }
        }
    }
    std::cout << library << " OverviewData files: " << count << " compressed blobs\n";
    check(count >= expectAtLeast, library + " OverviewData: fewer overview files than expected");
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_blob_check_fixtures_test <tests/fixtures>\n";
        return 2;
    }
    const fs::path fixtures = argv[1];
    const fs::path scratch = seabass::testing::scratchRoot() / "engine_blob_check_fixtures_test";
    fs::remove_all(scratch);

    const std::vector<std::string> all = {"trackData", "beatData", "overviewWaveFormData", "quickCues"};
    checkDatabase(fixtures, "anonymized_library/engine/Database2/m.db", all, scratch);
    // No analysed rows: its three trackData blobs are x'000102', too short
    // to be compressed (the check refuses them, as it must).
    checkDatabase(fixtures, "one_stick_two_catalogs/engine/Database2/m.db", {"quickCues"}, scratch);
    checkDatabase(fixtures, "duplicate_engine_library/Database2/m.db", all, scratch);
    for (const char *db : {"m-after-R1b.db", "m-after-R3c-save1.db", "m-after-R3d.db", "m-before-R3.db"}) {
        checkDatabase(fixtures, std::string("testshark_t73/engine/") + db, all, scratch);
    }
    checkOverviewFiles(fixtures, "anonymized_library/engine", 600);

    fs::remove_all(scratch);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "engine_blob_check_fixtures_test: all blobs accepted and decodable\n";
    return 0;
}
