// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// One damaged row never hangs, crashes or stops a pass over an Engine
// library (#107). libdjinterop, as pinned, loops for ever on a zlib stream
// that does not end within its blob, reserves whatever length a blob
// declares, inflates without a bound, and sizes or loops by counts it
// does not hold against the record. Seabass checks a row's blobs before
// it hands them over.
//
// On a copy of the anonymized fixture's 3.x library, one analysed row per
// (column, damage): trackData, beatData, overviewWaveFormData and
// quickCues each cut short (the stream does not end: the hang), cut to
// three bytes, declaring a length far past the data, garbage after the
// length, a small stream inflating to megabytes, and, where the record
// has one, a count far past it; loops (stored as they are) cut short,
// with such a count, and garbage. Then, each within a time limit:
//   - the scan reads every row; every other row reads as on the
//     undamaged copy, an overview or beat grid row's cues too; every
//     damaged row is named in a warning with its column;
//   - the cue stage (fillCues with a sample rate source) finishes;
//   - Library Health's sample rate audit finishes and names every row
//     whose trackData is damaged; its repair finishes, skips every row
//     whose records it would rewrite are damaged and repairs the rest;
//   - the waveform and beat grid readers come back for every damaged row;
//   - a cue write on a row with damaged cues is refused as a damaged row
//     (DamagedEngineRowError) naming the column and the title, and on any
//     other row goes through; a bpm and key write on a row with damaged
//     trackData is refused the same way before the bpm is written.
// The same, shorter, on a 2.x and a 1.x library libdjinterop creates, the
// 1.x one with its high-resolution waveform cut short too.
//
// And: a copy of the fixture with a hot journal beside m.db is refused by
// the guard opening the real file (never decoded unchecked), while the
// beat grid and waveform readers read their read copy (#110), the journal
// rolled back in it, check the blobs there, and leave the stick's journal
// alone; a per-beat grid of
// a six-hour track at 120 BPM (43201 markers, past the 1.x decoder's
// 32768) is no damaged blob on 3.x, and reads.
//
// A pass that does not come back within its limit fails the test at once
// with its name: without the check the first cut-short stream hangs.
//
// usage: engine_damaged_blob_test <tests/fixtures>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>
#include <zlib.h>

#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/engine_blob_check.hpp"
#include "infrastructure/engine/engine_sample_rates.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace std::chrono_literals;

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

// Runs `body`; a body that has not come back after `limit` ends the test
// as failed, naming it. A hang cannot be caught any other way.
void within(const std::string &what, std::chrono::seconds limit, const std::function<void()> &body)
{
    std::mutex lock;
    std::condition_variable done;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> held(lock);
        if (!done.wait_for(held, limit, [&] { return finished; })) {
            std::cerr << "FAILED: " << what << " did not finish within " << limit.count() << " s (hung)\n";
            std::cerr.flush();
            std::_Exit(1);
        }
    });
    const auto start = std::chrono::steady_clock::now();
    body();
    {
        std::lock_guard<std::mutex> held(lock);
        finished = true;
    }
    done.notify_one();
    watchdog.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::cout << "  " << what << ": " << ms.count() << " ms\n";
}

class Warnings : public application::ProgressReporter
{
public:
    void start(const std::string &, size_t) override {}
    void tick(size_t) override {}
    void finish() override {}
    void warn(const std::string &message) override { messages.push_back(message); }
    // Whether a warning names the row and the column.
    bool names(std::int64_t id, const std::string &column) const
    {
        const std::string row = "track id=" + std::to_string(id);
        for (const std::string &m : messages) {
            const auto at = m.find(row);
            // "track id=12" is not "track id=123".
            if (at != std::string::npos && (at + row.size() == m.size() || !std::isdigit(static_cast<unsigned char>(m[at + row.size()])))
                && m.find(column) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    std::vector<std::string> messages;
};

std::string read(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// --- bytes -----------------------------------------------------------------

std::string be32(std::uint32_t v)
{
    return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}

std::string be64(std::uint64_t v)
{
    std::string s(8, '\0');
    for (int i = 7; i >= 0; --i, v >>= 8) {
        s[static_cast<std::size_t>(i)] = static_cast<char>(v & 0xFF);
    }
    return s;
}

std::string le64(std::uint64_t v)
{
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i, v >>= 8) {
        s[static_cast<std::size_t>(i)] = static_cast<char>(v & 0xFF);
    }
    return s;
}

std::string deflated(const std::string &record)
{
    uLongf size = compressBound(static_cast<uLong>(record.size()));
    std::string out(size, '\0');
    if (compress2(reinterpret_cast<Bytef *>(out.data()), &size, reinterpret_cast<const Bytef *>(record.data()),
                  static_cast<uLong>(record.size()), Z_DEFAULT_COMPRESSION)
        != Z_OK) {
        throw std::runtime_error("compress2 failed");
    }
    out.resize(size);
    return out;
}

// An Engine blob: the record's length, then the record deflated.
std::string engineBlob(const std::string &record)
{
    return be32(static_cast<std::uint32_t>(record.size())) + deflated(record);
}

std::string inflatedRecord(const std::string &blob)
{
    const auto declared = (static_cast<std::uint32_t>(static_cast<unsigned char>(blob[0])) << 24)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(blob[1])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(blob[2])) << 8)
        | static_cast<std::uint32_t>(static_cast<unsigned char>(blob[3]));
    std::string out(declared, '\0');
    uLongf size = declared;
    if (uncompress(reinterpret_cast<Bytef *>(out.data()), &size, reinterpret_cast<const Bytef *>(blob.data() + 4),
                   static_cast<uLong>(blob.size() - 4))
            != Z_OK
        || size != declared) {
        throw std::runtime_error("a fixture blob does not inflate");
    }
    return out;
}

std::string garbage(std::size_t size, unsigned seed)
{
    std::mt19937 random(seed);
    std::string out(size, '\0');
    for (char &c : out) {
        c = static_cast<char>(random() & 0xFF);
    }
    return out;
}

const std::vector<std::string> &compressedColumns()
{
    static const std::vector<std::string> c = {"trackData", "beatData", "overviewWaveFormData", "quickCues"};
    return c;
}

// The record with its count, where it has one, far past what it holds.
std::optional<std::string> withCountPastTheRecord(const std::string &column, std::string record)
{
    if (column == "beatData") {
        // 24 bytes a marker: 24 times this is 24 again in 64 bits.
        record.replace(17, 8, be64(0x2000000000000001ull));
    } else if (column == "overviewWaveFormData" || column == "highResolutionWaveFormData") {
        record.replace(0, 8, be64(0x4000000000000000ull));
        record.replace(8, 8, be64(0x4000000000000000ull));
    } else if (column == "quickCues") {
        record.replace(0, 8, be64(1ull << 40));
    } else if (column == "loops") {
        record.replace(0, 8, le64(1ull << 40));
    } else {
        return std::nullopt;
    }
    return record;
}

// Every damage of a column, from the row's healthy blob.
std::vector<std::pair<std::string, std::string>> damagesOf(const std::string &column, const std::string &healthy)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (column == "loops") {
        out.emplace_back("cut short", healthy.substr(0, healthy.size() / 2));
        out.emplace_back("count past the record", *withCountPastTheRecord(column, healthy));
        out.emplace_back("garbage", garbage(healthy.size(), 7));
        return out;
    }
    const std::string record = inflatedRecord(healthy);
    out.emplace_back("stream cut short", healthy.substr(0, std::max<std::size_t>(6, healthy.size() / 2)));
    out.emplace_back("three bytes", healthy.substr(0, 3));
    out.emplace_back("length far past the data", be32(0x7FFFFFF0u) + healthy.substr(4));
    out.emplace_back("garbage", healthy.substr(0, 4) + garbage(healthy.size() - 4, 11));
    out.emplace_back("inflating to megabytes", healthy.substr(0, 4) + deflated(std::string(8u << 20, '\0')));
    if (const auto counted = withCountPastTheRecord(column, record)) {
        out.emplace_back("count past the record", engineBlob(*counted));
    }
    return out;
}

// --- the database ------------------------------------------------------------

struct Layout
{
    fs::path file;
    std::string table;
    std::string idColumn;
};

Layout layoutOf(const fs::path &library)
{
    if (fs::exists(library / "Database2" / "m.db")) {
        sqlite3 *db = nullptr;
        sqlite3_open_v2(pathToUtf8(library / "Database2" / "m.db").c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
        sqlite3_stmt *stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT count(*) FROM pragma_table_info('Track') WHERE name = 'trackData';", -1, &stmt,
                           nullptr);
        sqlite3_step(stmt);
        const bool inTrack = sqlite3_column_int(stmt, 0) == 1;
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return inTrack ? Layout{library / "Database2" / "m.db", "Track", "id"}
                       : Layout{library / "Database2" / "m.db", "PerformanceData", "trackId"};
    }
    return {library / "p.db", "PerformanceData", "id"};
}

std::optional<std::string> blobOf(const Layout &layout, const std::string &column, std::int64_t id)
{
    sqlite3 *db = nullptr;
    sqlite3_open_v2(pathToUtf8(layout.file).c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT " + column + " FROM " + layout.table + " WHERE " + layout.idColumn + " = ?1;";
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    std::optional<std::string> out;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_BLOB) {
        out = std::string(static_cast<const char *>(sqlite3_column_blob(stmt, 0)),
                          static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

void setBlob(const Layout &layout, const std::string &column, std::int64_t id, const std::string &bytes)
{
    sqlite3 *db = nullptr;
    sqlite3_open_v2(pathToUtf8(layout.file).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "UPDATE " + layout.table + " SET " + column + " = ?1 WHERE " + layout.idColumn + " = ?2;";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("cannot damage: ") + sqlite3_errmsg(db));
    }
    sqlite3_bind_blob(stmt, 1, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, id);
    if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db) != 1) {
        throw std::runtime_error("cannot damage row " + std::to_string(id) + ": " + sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

// Analysed rows: every column holds a blob.
std::vector<std::int64_t> analysedRows(const Layout &layout)
{
    sqlite3 *db = nullptr;
    sqlite3_open_v2(pathToUtf8(layout.file).c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT " + layout.idColumn + " FROM " + layout.table
        + " WHERE length(trackData) > 4 AND length(beatData) > 4 AND length(overviewWaveFormData) > 4"
          " AND length(quickCues) > 4 AND length(loops) >= 8 ORDER BY 1;";
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    std::vector<std::int64_t> ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ids;
}

struct Damaged
{
    std::int64_t id;
    std::string column;
    std::string damage;
};

// One row per (column, damage), in row order.
std::vector<Damaged> damageRows(const fs::path &library, const std::vector<std::string> &columns)
{
    const Layout layout = layoutOf(library);
    const std::vector<std::int64_t> rows = analysedRows(layout);
    std::vector<Damaged> damaged;
    std::size_t next = 0;
    for (const std::string &column : columns) {
        // Each damage from a healthy row of its own.
        const std::optional<std::string> healthy = blobOf(layout, column, rows.at(next));
        if (!healthy) {
            throw std::runtime_error("no " + column + " to damage");
        }
        for (const auto &[damage, bytes] : damagesOf(column, *healthy)) {
            const std::int64_t id = rows.at(next++);
            setBlob(layout, column, id, bytes);
            damaged.push_back({id, column, damage});
        }
    }
    return damaged;
}

std::string digest(const domain::Track &t)
{
    std::ostringstream out;
    out << t.title << "|" << t.filename << "|" << t.bpm << "|" << t.key << "|" << t.cueRateUnverified;
    for (const domain::CuePoint &c : t.cues) {
        out << "|" << static_cast<int>(c.kind) << ":" << c.hotCueNumber << ":" << std::llround(c.positionMs * 100) << ":"
            << c.isLoop << ":" << std::llround(c.loopEndMs * 100);
    }
    return out.str();
}

std::map<std::string, domain::Track> byId(const std::vector<domain::Track> &tracks)
{
    std::map<std::string, domain::Track> out;
    for (const domain::Track &t : tracks) {
        out[t.sourceId] = t;
    }
    return out;
}

std::vector<domain::Track> scan(const fs::path &library, Warnings &warnings, const std::string &what)
{
    std::vector<domain::Track> tracks;
    within(what, 120s, [&] {
        infrastructure::engine::LibdjinteropEngineReader reader(pathToUtf8(library));
        reader.setProgressReporter(warnings);
        tracks = reader.readAll();
    });
    return tracks;
}

bool cueColumn(const std::string &column)
{
    return column == "quickCues" || column == "loops";
}

// The scan, the readers and the cue writer over a library with `damaged`
// rows; `pristine` is the same library undamaged.
void checkLibrary(const std::string &name, const fs::path &pristine, const fs::path &library,
                  const std::vector<Damaged> &damaged, bool withSampleRateAudit)
{
    std::cout << name << ": " << damaged.size() << " damaged rows\n";
    Warnings none;
    const auto before = byId(scan(pristine, none, name + ": the scan of the undamaged copy"));

    // The scan.
    Warnings warnings;
    std::vector<domain::Track> read = scan(library, warnings, name + ": the scan");
    const auto after = byId(read);
    check(after.size() == before.size(), name + ": the scan read " + std::to_string(after.size()) + " rows of "
                                             + std::to_string(before.size()));
    std::set<std::string> damagedIds;
    for (const Damaged &d : damaged) {
        const std::string id = std::to_string(d.id);
        damagedIds.insert(id);
        const std::string what = name + ": row " + id + " (" + d.column + ", " + d.damage + ")";
        check(warnings.names(d.id, d.column), what + " is not named with its column in the scan's warnings");
        const auto found = after.find(id);
        if (found == after.end()) {
            check(false, what + " was not read at all");
            continue;
        }
        check(found->second.title == before.at(id).title && found->second.filename == before.at(id).filename,
              what + ": its title or path did not read");
        // Its cues come from trackData, quickCues and loops alone.
        if (d.column == "beatData" || d.column == "overviewWaveFormData" || d.column == "highResolutionWaveFormData") {
            check(digest(found->second) == digest(before.at(id)), what + ": its cues did not read as before");
        }
    }
    int same = 0;
    for (const auto &[id, track] : before) {
        if (damagedIds.contains(id)) {
            continue;
        }
        const auto found = after.find(id);
        const bool equal = found != after.end() && digest(found->second) == digest(track);
        check(equal, name + ": undamaged row " + id + " did not read as before");
        same += equal ? 1 : 0;
    }
    std::cout << "  " << same << " other rows read as on the undamaged copy\n";

    // The cue stage, asking a source for every row without a stored rate.
    within(name + ": the cue stage", 120s, [&] {
        infrastructure::engine::LibdjinteropEngineReader reader(pathToUtf8(library));
        Warnings stage;
        reader.setProgressReporter(stage);
        reader.setSampleRateSource([](const std::string &) { return std::optional<double>(44100.0); });
        reader.fillCues(read);
    });

    // The readers a page opens a row with.
    for (const Damaged &d : damaged) {
        within(name + ": the waveform and beat grid of row " + std::to_string(d.id) + " (" + d.column + ", " + d.damage
                   + ")",
               30s, [&] {
                   (void)infrastructure::engine::readTrackAnalysis(pathToUtf8(library), std::to_string(d.id), 100000);
                   (void)infrastructure::engine::readBeatGrid(pathToUtf8(library), std::to_string(d.id));
               });
    }

    if (withSampleRateAudit) {
        Warnings auditWarnings;
        infrastructure::engine::SampleRateAudit audit;
        within(name + ": the sample rate audit", 120s, [&] {
            audit = infrastructure::engine::auditSampleRates(pathToUtf8(library), {},
                                                             application::CancellationToken::none(), auditWarnings);
        });
        check(audit.error.empty(), name + ": the audit failed: " + audit.error);
        check(audit.tracksChecked == static_cast<int>(before.size()), name + ": the audit did not check every row");
        for (const Damaged &d : damaged) {
            if (d.column == "trackData") {
                check(auditWarnings.names(d.id, d.column),
                      name + ": the audit does not name row " + std::to_string(d.id) + " (" + d.damage + ")");
            }
        }
        // The repair, on every damaged row and one healthy one.
        std::vector<infrastructure::engine::SampleRateEntry> entries;
        int expectRepaired = 0;
        for (const Damaged &d : damaged) {
            infrastructure::engine::SampleRateEntry entry;
            entry.trackId = d.id;
            entry.sampleRateFromFile = 44100.0;
            entries.push_back(entry);
            expectRepaired += cueColumn(d.column) ? 1 : 0;
        }
        for (const auto &[id, track] : before) {
            if (!damagedIds.contains(id)) {
                infrastructure::engine::SampleRateEntry entry;
                entry.trackId = std::stoll(id);
                entry.sampleRateFromFile = 44100.0;
                entries.push_back(entry);
                ++expectRepaired;
                break;
            }
        }
        infrastructure::engine::SampleRateRepair repair;
        within(name + ": the sample rate repair", 120s,
               [&] { repair = infrastructure::engine::repairSampleRates(pathToUtf8(library), entries); });
        check(repair.error.empty(), name + ": the repair failed: " + repair.error);
        check(repair.repaired == expectRepaired, name + ": the repair repaired " + std::to_string(repair.repaired)
                                                     + " rows, not " + std::to_string(expectRepaired));
    }

    // The cue writer: a hot cue at one second on every damaged row.
    infrastructure::engine::LibdjinteropEngineCueWriter writer(pathToUtf8(library));
    for (const Damaged &d : damaged) {
        const std::string what = name + ": a cue write on row " + std::to_string(d.id) + " (" + d.column + ", " + d.damage + ")";
        const std::string title = before.at(std::to_string(d.id)).title;
        std::string refused;
        bool refusedAsDamaged = false;
        within(what, 30s, [&] {
            domain::CuePoint cue;
            cue.kind = domain::CuePoint::Kind::Hot;
            cue.hotCueNumber = 1;
            cue.positionMs = 1000.0;
            cue.comment = "Cue";  // 1.x refuses a hot cue without a label
            try {
                writer.writeHotCues(std::to_string(d.id), {cue});
            } catch (const infrastructure::engine::DamagedEngineRowError &e) {
                refused = e.what();
                refusedAsDamaged = true;
            } catch (const std::exception &e) {
                refused = e.what();
            }
        });
        if (cueColumn(d.column)) {
            check(refusedAsDamaged && refused.find(d.column) != std::string::npos
                      && refused.find(title) != std::string::npos,
                  what + " was not refused as a damaged row naming the column and the title: " + refused);
        } else {
            check(refused.empty(), what + " was refused: " + refused);
        }
        if (d.column == "trackData") {
            // The bpm and the key together: refused before either is
            // written, so the row keeps the bpm it had.
            const auto bpmOf = [&] {
                auto db = djinterop::engine::load_database(pathToUtf8(library));
                return db.track_by_id(d.id)->bpm();
            };
            const std::optional<double> bpmBefore = bpmOf();
            std::string keyRefused;
            bool keyRefusedAsDamaged = false;
            within(what + ", its bpm and key", 30s, [&] {
                try {
                    writer.propagateMissingFields(std::to_string(d.id), 133.0, std::string("Am"));
                } catch (const infrastructure::engine::DamagedEngineRowError &e) {
                    keyRefused = e.what();
                    keyRefusedAsDamaged = true;
                } catch (const std::exception &e) {
                    keyRefused = e.what();
                }
            });
            check(keyRefusedAsDamaged && keyRefused.find("trackData") != std::string::npos
                      && keyRefused.find(title) != std::string::npos,
                  what + ": a bpm and key write was not refused as a damaged row naming trackData and the title: "
                      + keyRefused);
            check(bpmOf() == bpmBefore, what + ": the bpm was written before the key was refused");
        }
    }
}

// A library libdjinterop creates at `schema`, with `count` analysed rows
// carrying a grid, a waveform, hot cues, a loop and a main cue.
void createLibrary(const fs::path &library, const djinterop::engine::engine_schema &schema, int count)
{
    auto db = djinterop::engine::create_database(pathToUtf8(library), schema);
    const double rate = 44100.0;
    const unsigned long long samples = 44100ull * 90;
    const auto extents = djinterop::engine::calculate_high_resolution_waveform_extents(samples, rate);
    for (int i = 1; i <= count; ++i) {
        djinterop::track_snapshot s;
        s.title = "Track " + std::to_string(i);
        s.relative_path = "../Contents/track-" + std::to_string(i) + ".flac";
        s.sample_rate = rate;
        s.sample_count = samples;
        s.bpm = 120.0;
        s.duration = std::chrono::milliseconds(90000);
        s.beatgrid = {{0, 0.0}, {180, 180 * rate / 2.0}};
        s.waveform.assign(extents.size, djinterop::waveform_entry{{10, 255}, {20, 255}, {30, 255}});
        s.hot_cues.assign(8, std::nullopt);
        s.hot_cues[0] = djinterop::hot_cue{"Drop", rate * 10, djinterop::engine::standard_pad_colors::pad_1};
        s.loops.assign(8, std::nullopt);
        s.loops[0] = djinterop::loop{"Loop", rate * 20, rate * 22, djinterop::engine::standard_pad_colors::pad_2};
        s.main_cue = rate * 5;
        db.create_track(s);
    }
}

void exec(sqlite3 *db, const std::string &sql)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string why = error ? error : "?";
        sqlite3_free(error);
        throw std::runtime_error("sqlite: " + why + " in: " + sql);
    }
}

// A copy of the fixture's library whose m.db carries a hot journal (a save
// cut off mid-transaction: pages spilled into m.db, the journal beside it),
// made as engine_pending_journal_test makes one. Its blobs may be half
// written: the guard, opening the real file, refuses to vouch for them.
// The readers read a read copy with the journal rolled back in it (#110)
// and check the blobs there, in the database they decode.
void checkHotJournal(const fs::path &fixtures, const fs::path &scratch)
{
    std::cout << "hot journal\n";
    const fs::path live = scratch / "journal-live";
    const fs::path crashed = scratch / "journal-crashed";
    fs::copy(fixtures / "anonymized_library" / "engine", live, fs::copy_options::recursive);
    fs::copy(live, crashed, fs::copy_options::recursive);
    const std::int64_t row = analysedRows(layoutOf(live)).at(0);
    {
        sqlite3 *db = nullptr;
        sqlite3_open(pathToUtf8(live / "Database2" / "m.db").c_str(), &db);
        exec(db, "PRAGMA journal_mode=DELETE");
        exec(db, "PRAGMA cache_size=1");
        exec(db, "PRAGMA cache_spill=1");
        exec(db, "BEGIN");
        exec(db, "UPDATE Track SET title = 'INTERRUPTED', path = hex(randomblob(400))");
        fs::copy_file(live / "Database2" / "m.db", crashed / "Database2" / "m.db", fs::copy_options::overwrite_existing);
        fs::copy_file(live / "Database2" / "m.db-journal", crashed / "Database2" / "m.db-journal",
                      fs::copy_options::overwrite_existing);
        exec(db, "ROLLBACK");
        sqlite3_close(db);
    }
    std::string error;
    const std::optional<infrastructure::engine::EngineBlobs> damaged =
        infrastructure::engine::EngineBlobGuard(pathToUtf8(crashed)).damaged(row, &error);
    std::cout << "  the guard says: " << (damaged ? std::string("checked") : error) << "\n";
    check(!damaged, "hot journal: the guard read the blobs of a database with a hot journal");
    check(!damaged && !infrastructure::engine::engineBlobGuardFailureReadsUnchecked(error),
          "hot journal: the failure would have the blobs decoded unchecked: " + error);
    check(fs::exists(crashed / "Database2" / "m.db-journal"), "hot journal: the check rolled the journal back");
    // The readers read a read copy (#110), the journal rolled back in the
    // copy, and check the blobs in that copy, the database they decode:
    // what they read is the library as it was before the cut-off save,
    // and the stick's own m.db and journal are left as they are.
    const std::string journalBefore = read(crashed / "Database2" / "m.db-journal");
    std::size_t beats = 0;
    within("hot journal: the beat grid and waveform readers", 30s, [&] {
        beats = infrastructure::engine::readBeatGrid(pathToUtf8(crashed), std::to_string(row)).size();
        (void)infrastructure::engine::readTrackAnalysis(pathToUtf8(crashed), std::to_string(row), 100000);
    });
    check(beats > 0 && beats == infrastructure::engine::readBeatGrid(pathToUtf8(live), std::to_string(row)).size(),
          "hot journal: the beat grid reader did not read the library as it was before the cut-off save");
    check(read(crashed / "Database2" / "m.db-journal") == journalBefore,
          "hot journal: a reader changed or rolled back the stick's journal");

    // A WAL database a read-only open cannot open (a read-only mount, its
    // log unread): SQLite says "unable to open database file". Refused too.
    // Asked of the rule directly: a read-only mount cannot be made here
    // without permission tricks, which root ignores.
    check(!infrastructure::engine::engineBlobGuardFailureReadsUnchecked(
              "could not open /stick/Engine Library/Database2/m.db: unable to open database file"),
          "a WAL database that cannot be opened read-only would have its blobs decoded unchecked");
    check(infrastructure::engine::engineBlobGuardFailureReadsUnchecked(
              "could not list the columns of Track: no such table: Track"),
          "a schema the check does not know is no reason to refuse");
}

// A grid with a marker on every beat of a six-hour track at 120 BPM: far
// past the 32768 markers libdjinterop's 1.x decoder allows, and a sound
// 3.x record all the same. Not taken for damaged: the scan reads it with
// no warning about its blobs.
void checkLongGrid(const fs::path &scratch)
{
    std::cout << "long grid\n";
    const fs::path library = scratch / "long-grid" / "Engine Library";
    fs::create_directories(library.parent_path());
    const double rate = 44100.0;
    const unsigned long long samples = 44100ull * 6 * 3600;
    const int beats = 6 * 3600 * 2;
    {
        auto db = djinterop::engine::create_database(pathToUtf8(library));
        djinterop::track_snapshot s;
        s.title = "Six Hours";
        s.relative_path = "../Contents/six-hours.flac";
        s.sample_rate = rate;
        s.sample_count = samples;
        s.bpm = 120.0;
        s.duration = std::chrono::milliseconds(6ll * 3600 * 1000);
        for (int beat = 0; beat <= beats; ++beat) {
            s.beatgrid.push_back({beat, beat * rate / 2.0});
        }
        db.create_track(s);
    }
    const Layout layout = layoutOf(library);
    const std::optional<std::string> beatData = blobOf(layout, "beatData", 1);
    check(beatData.has_value(), "long grid: no beat data written");
    if (beatData) {
        const std::string record = inflatedRecord(*beatData);
        const std::uint64_t markers = static_cast<std::uint64_t>(static_cast<unsigned char>(record[17 + 4])) << 24
            | static_cast<std::uint64_t>(static_cast<unsigned char>(record[17 + 5])) << 16
            | static_cast<std::uint64_t>(static_cast<unsigned char>(record[17 + 6])) << 8
            | static_cast<std::uint64_t>(static_cast<unsigned char>(record[17 + 7]));
        std::cout << "  " << markers << " markers, " << record.size() << " bytes\n";
        check(markers > 32768, "long grid: the grid was written with " + std::to_string(markers) + " markers");
        check(infrastructure::engine::internal::engineBlobDecodable(infrastructure::engine::EngineBlob::BeatData,
                                                                     *beatData),
              "long grid: a six-hour per-beat grid was taken for damaged");
    }
    Warnings warnings;
    const std::vector<domain::Track> tracks = scan(library, warnings, "long grid: the scan");
    check(tracks.size() == 1, "long grid: the row was not read");
    check(!warnings.names(1, "beatData"), "long grid: the scan named the row's beat data as damaged");
    check(infrastructure::engine::readBeatGrid(pathToUtf8(library), "1").size() > 32768,
          "long grid: the beat grid reader did not read the grid");
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_damaged_blob_test <tests/fixtures>\n";
        return 2;
    }
    const fs::path fixtures = argv[1];
    const fs::path scratch = testing::scratchRoot() / "engine_damaged_blob_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    try {
        // 3.x: the anonymized fixture's library, every column.
        {
            const fs::path pristine = scratch / "v3-pristine";
            const fs::path library = scratch / "v3";
            fs::copy(fixtures / "anonymized_library" / "engine", pristine, fs::copy_options::recursive);
            fs::copy(pristine, library, fs::copy_options::recursive);
            std::vector<std::string> columns = compressedColumns();
            columns.push_back("loops");
            const std::vector<Damaged> damaged = damageRows(library, columns);
            checkLibrary("3.x", pristine, library, damaged, true);
        }
        // 2.x and 1.x: created, every column cut short and the loops' count.
        const std::vector<std::pair<std::string, djinterop::engine::engine_schema>> created = {
            {"2.x", djinterop::engine::engine_schema::schema_2_21_2},
            {"1.x", djinterop::engine::engine_schema::schema_1_18_0_desktop},
        };
        for (const auto &[name, schema] : created) {
            const fs::path pristine = scratch / (name + "-pristine") / "Engine Library";
            const fs::path library = scratch / name / "Engine Library";
            fs::create_directories(pristine.parent_path());
            createLibrary(pristine, schema, 12);
            fs::create_directories(library.parent_path());
            fs::copy(pristine, library, fs::copy_options::recursive);
            const Layout layout = layoutOf(library);
            const std::vector<std::int64_t> rows = analysedRows(layout);
            std::vector<std::string> columns = compressedColumns();
            if (name == "1.x") {
                columns.push_back("highResolutionWaveFormData");
            }
            std::vector<Damaged> damaged;
            std::size_t next = 0;
            for (const std::string &column : columns) {
                const std::string healthy = *blobOf(layout, column, rows.at(next));
                const std::int64_t id = rows.at(next++);
                setBlob(layout, column, id, healthy.substr(0, std::max<std::size_t>(6, healthy.size() / 2)));
                damaged.push_back({id, column, "stream cut short"});
            }
            {
                const std::string healthy = *blobOf(layout, "loops", rows.at(next));
                const std::int64_t id = rows.at(next++);
                setBlob(layout, "loops", id, *withCountPastTheRecord("loops", healthy));
                damaged.push_back({id, "loops", "count past the record"});
            }
            checkLibrary(name, pristine, library, damaged, name == "2.x");
        }
        checkHotJournal(fixtures, scratch);
        checkLongGrid(scratch);
    } catch (const std::exception &e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        ++failures;
    }

    fs::remove_all(scratch);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "engine_damaged_blob_test: all cases passed\n";
    return 0;
}
