// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// For the copy-beatgrids --waveforms CLI tests: what the analysis columns
// and overview files of an Engine library hold, decoded with libdjinterop's
// own blob code. Prints
//
//   rows: <Track rows>
//   analysed: <rows with isAnalyzed = 1>
//   records of 68 bytes: <trackData that inflates to 68 bytes>
//   records with zero loudness: <n>
//   overviews of 1024 points: <overviewWaveFormData that decodes so>
//   beat grids set: <beatData with is_beatgrid_set = 1>
//   overview files: <.rgb files in the database's uuid folder>
//   overview files matching their row: <ones that decode to 1024 points
//     over their row's own length, samples / 1024 a point>
//
// and leaves the matching to the test's PASS_REGULAR_EXPRESSION. The
// database is opened read-only and immutable: nothing beside it changes.
//
// usage: engine_analysis_rows <Engine Library folder>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <djinterop/engine/v3/overview_waveform_data_blob.hpp>
#include <djinterop/engine/v3/track_data_blob.hpp>
#include <djinterop/engine/v3/beat_data_blob.hpp>
#include <sqlite3.h>
#include <zlib.h>

#include "infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;
using Overview = djinterop::engine::v3::overview_waveform_data_blob;
using TrackData = djinterop::engine::v3::track_data_blob;
using BeatData = djinterop::engine::v3::beat_data_blob;

namespace
{

std::size_t inflatedSize(const std::vector<std::byte> &blob)
{
    if (blob.size() < 4) {
        return 0;
    }
    const auto *b = reinterpret_cast<const unsigned char *>(blob.data());
    uLongf size = (uLongf(b[0]) << 24) | (uLongf(b[1]) << 16) | (uLongf(b[2]) << 8) | uLongf(b[3]);
    std::vector<unsigned char> out(size);
    if (uncompress(out.data(), &size, b + 4, static_cast<uLong>(blob.size() - 4)) != Z_OK) {
        return 0;
    }
    return size;
}

std::vector<std::byte> column(sqlite3_stmt *stmt, int c)
{
    if (sqlite3_column_type(stmt, c) == SQLITE_NULL) {
        return {};
    }
    const auto *bytes = static_cast<const std::byte *>(sqlite3_column_blob(stmt, c));
    return {bytes, bytes + sqlite3_column_bytes(stmt, c)};
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: engine_analysis_rows <Engine Library folder>\n";
        return 2;
    }
    const fs::path root = seabass::pathFromUtf8(argv[1]);
    const std::string file = seabass::pathToUtf8(root / "Database2" / "m.db");
    sqlite3 *handle = nullptr;
    const std::string uri = "file:" + file + "?immutable=1";
    if (sqlite3_open_v2(uri.c_str(), &handle, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        std::cerr << "cannot open " << file << "\n";
        return 2;
    }
    std::string uuid;
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(handle, "SELECT uuid FROM Information", -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        uuid = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);

    int rows = 0, analysed = 0, records68 = 0, zeroLoudness = 0, overviews = 0, gridsSet = 0;
    std::map<std::int64_t, double> samplesOf;
    sqlite3_prepare_v2(handle,
                       "SELECT t.id, t.isAnalyzed, p.trackData, p.overviewWaveFormData, p.beatData FROM Track t"
                       " LEFT JOIN PerformanceData p ON p.trackId = t.id",
                       -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ++rows;
        const std::int64_t id = sqlite3_column_int64(stmt, 0);
        analysed += sqlite3_column_int(stmt, 1) == 1;
        const auto record = column(stmt, 2);
        if (inflatedSize(record) == 68) {
            ++records68;
        }
        try {
            if (!record.empty()) {
                const TrackData data = TrackData::from_blob(record);
                samplesOf[id] = static_cast<double>(data.samples);
                zeroLoudness += data.average_loudness_low == 0.0 && data.average_loudness_mid == 0.0
                    && data.average_loudness_high == 0.0;
            }
            const auto overview = column(stmt, 3);
            if (!overview.empty() && Overview::from_blob(overview).waveform_points.size() == 1024) {
                ++overviews;
            }
            const auto beats = column(stmt, 4);
            if (!beats.empty() && BeatData::from_blob(beats).is_beatgrid_set == 1) {
                ++gridsSet;
            }
        } catch (const std::exception &) {
            // Not counted: a blob that does not decode is none of these.
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);

    int files = 0, matching = 0;
    const fs::path dir = root / "Database2" / "OverviewData" / seabass::pathFromUtf8(uuid);
    std::error_code ec;
    if (!uuid.empty() && fs::is_directory(dir, ec)) {
        for (const auto &entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() != ".rgb") {
                continue;
            }
            ++files;
            std::ifstream in(entry.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::vector<std::byte> bytes(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                bytes[i] = static_cast<std::byte>(text[i]);
            }
            try {
                const Overview overview = Overview::from_blob(bytes);
                const auto row = samplesOf.find(std::stoll(seabass::pathToUtf8(entry.path().stem())));
                if (overview.waveform_points.size() == 1024 && row != samplesOf.end()
                    && overview.samples_per_waveform_point == row->second / 1024.0) {
                    ++matching;
                }
            } catch (const std::exception &) {
            }
        }
    }

    std::cout << "rows: " << rows << "\nanalysed: " << analysed << "\nrecords of 68 bytes: " << records68
              << "\nrecords with zero loudness: " << zeroLoudness << "\noverviews of 1024 points: " << overviews
              << "\nbeat grids set: " << gridsSet << "\noverview files: " << files
              << "\noverview files matching their row: " << matching << "\n";
    return 0;
}
