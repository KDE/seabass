// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// What the Sync after Rekordbox Export change tests share: a stick made of
// a COPY of tests/fixtures/anonymized_library (PIONEER from its rekordbox/
// folder, "Engine Library" from its engine/ folder; never the fixture in
// place), plain SQL looks at a database, a whole-database dump to compare
// before and after, the save loop and its undo.

#include <QString>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "fixture_copy.hpp"
#include "gui/edit/changes/restore_backups_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace seabass::testing
{

struct EngineChangeStick
{
    std::filesystem::path root;
    std::filesystem::path pioneer;
    std::filesystem::path engine;
    std::string db;  // the Engine library's m.db

    QString pioneerPath() const { return gui::pathToQString(pioneer); }
    QString enginePath() const { return gui::pathToQString(engine); }
    std::string engineUtf8() const { return pathToUtf8(engine); }
};

// A fresh stick under the test's scratch root, named `name`. Backups and
// the Seabass home go under it too, so nothing reaches ~/Seabass.
inline EngineChangeStick makeEngineChangeStick(const std::filesystem::path &fixture, const std::string &name)
{
    namespace fs = std::filesystem;
    EngineChangeStick stick;
    stick.root = scratchRoot() / name;
    fs::remove_all(stick.root);
    fs::create_directories(stick.root);
    stick.pioneer = stick.root / "PIONEER";
    stick.engine = stick.root / "Engine Library";
    std::error_code ec;
    copyPioneerFixture(fixture / "rekordbox", stick.pioneer, ec);
    assert(!ec && "the rekordbox side of the fixture copies");
    fs::copy(fixture / "engine", stick.engine, fs::copy_options::recursive, ec);
    assert(!ec && "the Engine side of the fixture copies");
    stick.db = pathToUtf8(stick.engine / "Database2" / "m.db");
    return stick;
}

// Engine's import counter put level with export.pdb (15132 in the
// fixture, against Engine's 14204), as any earlier Seabass save with both
// catalogs leaves a stick. Then a save with both catalogs leaves the
// counter alone, and its Undo is byte for byte.
inline void levelImportCounter(const EngineChangeStick &stick)
{
    const auto state = infrastructure::engine::readRekordboxImportState(pathToUtf8(stick.engine), pathToUtf8(stick.pioneer));
    assert(state.error.empty() && state.librarySequence == 15132 && state.engineCounter == 14204);
    std::string error;
    const bool marked = infrastructure::engine::markRekordboxLibraryImported(pathToUtf8(stick.engine), 15132, &error);
    assert(marked && error.empty());
    (void)marked;
}

// Every row a query returns, each column as text (NULL as "<null>",
// blobs as hex), read-only.
inline std::vector<std::vector<std::string>> sqlRows(const std::string &file, const std::string &sql)
{
    sqlite3 *handle = nullptr;
    const int opened = sqlite3_open_v2(file.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr);
    assert(opened == SQLITE_OK);
    (void)opened;
    sqlite3_stmt *stmt = nullptr;
    const int prepared = sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr);
    if (prepared != SQLITE_OK) {
        std::cerr << sql << ": " << sqlite3_errmsg(handle) << "\n";
    }
    assert(prepared == SQLITE_OK);
    std::vector<std::vector<std::string>> out;
    static const char *Hex = "0123456789abcdef";
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::vector<std::string> row;
        for (int c = 0; c < sqlite3_column_count(stmt); ++c) {
            switch (sqlite3_column_type(stmt, c)) {
            case SQLITE_NULL:
                row.emplace_back("<null>");
                break;
            case SQLITE_BLOB: {
                const auto *bytes = static_cast<const unsigned char *>(sqlite3_column_blob(stmt, c));
                const int size = sqlite3_column_bytes(stmt, c);
                std::string hex = "x'";
                for (int i = 0; i < size; ++i) {
                    hex += Hex[bytes[i] >> 4];
                    hex += Hex[bytes[i] & 0xf];
                }
                row.push_back(hex + "'");
                break;
            }
            default:
                row.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(stmt, c)));
            }
        }
        out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return out;
}

inline std::int64_t sqlScalar(const std::string &file, const std::string &sql)
{
    const auto r = sqlRows(file, sql);
    assert(r.size() == 1 && r[0].size() == 1);
    return std::stoll(r[0][0]);
}

inline bool sqlExec(const std::string &file, const std::string &sql)
{
    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(file.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(handle);
        return false;
    }
    char *message = nullptr;
    const bool ok = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &message) == SQLITE_OK;
    if (!ok) {
        std::cerr << sql << ": " << (message ? message : "") << "\n";
    }
    sqlite3_free(message);
    sqlite3_close(handle);
    return ok;
}

// Every table of the database, each row as one line of its columns (rowid
// first), keyed by table. Two dumps that compare equal are the same rows
// in every table.
using DatabaseDump = std::map<std::string, std::vector<std::string>>;

inline DatabaseDump dumpDatabase(const std::string &file)
{
    DatabaseDump dump;
    for (const auto &t : sqlRows(file, "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name;")) {
        const std::string &table = t[0];
        // sqlite_sequence has no rowid alias worth ordering by, and a
        // WITHOUT ROWID table none at all; every other table here has one.
        const bool hasRowid = table != "sqlite_sequence";
        const std::string sql = hasRowid ? "SELECT rowid, * FROM \"" + table + "\" ORDER BY rowid;"
                                         : "SELECT * FROM \"" + table + "\" ORDER BY 1;";
        std::vector<std::string> lines;
        for (const auto &row : sqlRows(file, sql)) {
            std::string line;
            for (const auto &v : row) {
                line += v;
                line += '\x1f';
            }
            lines.push_back(std::move(line));
        }
        dump[table] = std::move(lines);
    }
    return dump;
}

// The tables whose rows differ between two dumps, with how many rows each
// side has that the other lacks.
struct DumpDifference
{
    std::string table;
    std::vector<std::string> onlyBefore;
    std::vector<std::string> onlyAfter;
};

inline std::vector<DumpDifference> differences(const DatabaseDump &before, const DatabaseDump &after)
{
    std::set<std::string> tables;
    for (const auto &[t, rows] : before) {
        tables.insert(t);
    }
    for (const auto &[t, rows] : after) {
        tables.insert(t);
    }
    std::vector<DumpDifference> out;
    for (const auto &t : tables) {
        const auto b = before.count(t) ? before.at(t) : std::vector<std::string>{};
        const auto a = after.count(t) ? after.at(t) : std::vector<std::string>{};
        const std::multiset<std::string> bs(b.begin(), b.end());
        const std::multiset<std::string> as(a.begin(), a.end());
        DumpDifference d{t, {}, {}};
        std::set_difference(bs.begin(), bs.end(), as.begin(), as.end(), std::back_inserter(d.onlyBefore));
        std::set_difference(as.begin(), as.end(), bs.begin(), bs.end(), std::back_inserter(d.onlyAfter));
        if (!d.onlyBefore.empty() || !d.onlyAfter.empty()) {
            out.push_back(std::move(d));
        }
    }
    return out;
}

inline void printDifferences(const std::vector<DumpDifference> &diffs)
{
    for (const auto &d : diffs) {
        std::cerr << "  " << d.table << ": " << d.onlyBefore.size() << " row(s) only before, " << d.onlyAfter.size()
                  << " only after\n";
    }
}

inline std::string fileBytes(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    assert(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// One save of `changes`, as LibraryEditSession runs it: a SaveContext over
// the stick's catalogs (either may be empty) and runSaveLoop.
inline gui::SaveLoopResult saveChanges(const std::vector<std::shared_ptr<gui::PendingChange>> &changes,
                                       const QString &rekordboxPath, const QString &enginePath)
{
    application::CancellationToken token;
    gui::SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, rekordboxPath, enginePath);
    return gui::runSaveLoop(changes, ctx);
}

// Undo Last Save, as the session runs it: RestoreBackupsChange through a
// save of its own over the same catalogs.
inline gui::SaveLoopResult undoSave(const gui::SaveLoopResult &saved, const QString &rekordboxPath,
                                    const QString &enginePath)
{
    assert(!saved.backups.empty() && "a save that wrote leaves backups to undo from");
    return saveChanges({std::make_shared<gui::RestoreBackupsChange>(saved.backups)}, rekordboxPath, enginePath);
}

// A playlist's track ids in chain order, the playlist found by its title
// among the roots (the fixture has no folders).
inline std::vector<std::int64_t> rootPlaylistTracks(const std::string &db, const std::string &title)
{
    const auto list = sqlRows(db, "SELECT id FROM Playlist WHERE parentListId = 0 AND title = '" + title + "';");
    assert(list.size() == 1);
    std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> entries;  // id -> (track, next)
    std::set<std::int64_t> pointedAt;
    for (const auto &r : sqlRows(db, "SELECT id, trackId, nextEntityId FROM PlaylistEntity WHERE listId = " + list[0][0] + ";")) {
        entries[std::stoll(r[0])] = {std::stoll(r[1]), std::stoll(r[2])};
        pointedAt.insert(std::stoll(r[2]));
    }
    std::vector<std::int64_t> heads;
    for (const auto &[id, e] : entries) {
        if (!pointedAt.count(id)) {
            heads.push_back(id);
        }
    }
    assert(entries.empty() || heads.size() == 1);
    std::vector<std::int64_t> order;
    for (std::int64_t at = entries.empty() ? 0 : heads[0]; at != 0; at = entries.at(at).second) {
        assert(order.size() <= entries.size() && "the chain does not loop");
        order.push_back(entries.at(at).first);
    }
    assert(order.size() == entries.size() && "the chain reaches every entry");
    return order;
}

// A PCM WAV of silence: 16-bit stereo, `seconds` long, at `rate` Hz.
inline void writeSilentWav(const std::filesystem::path &file, std::uint32_t rate, std::uint32_t seconds)
{
    const std::uint16_t channels = 2;
    const std::uint16_t bits = 16;
    const std::uint32_t byteRate = rate * channels * bits / 8;
    const std::uint32_t dataBytes = byteRate * seconds;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.put(static_cast<char>((v >> (8 * i)) & 0xff));
        }
    };
    const auto u16 = [&](std::uint16_t v) {
        out.put(static_cast<char>(v & 0xff));
        out.put(static_cast<char>(v >> 8));
    };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);  // PCM
    u16(channels);
    u32(rate);
    u32(byteRate);
    u16(static_cast<std::uint16_t>(channels * bits / 8));
    u16(bits);
    out.write("data", 4);
    u32(dataBytes);
    const std::string silence(dataBytes, '\0');
    out.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    assert(out.good());
}

}  // namespace seabass::testing
