// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// AnlzPathIndex decides which analysis file a cue write lands in. A wrong
// answer writes correct data into the wrong track: both tracks end up
// wrong, the save reports success because every write succeeded, and undo
// restores the file that was damaged rather than the edit that was meant.
//
// The index exists to replace findAnlzPathForTrackId(), which parses the
// whole database per call, so the property that matters is that the two
// never disagree: not for one track the writers happen to pick, but for
// every id the database holds, for every id it does not, and for a
// database that cannot be read at all. Issue #2.
//
// The exhaustive comparison is against one independent walk of the
// tracks table, not against the direct lookup per id: the direct lookup
// parses 1.4 MB per call, and 1161 of those cost 15 s to prove what the
// walk proves in 30 ms. The direct lookup is still called on a spread of
// ids, so the contract between the two is exercised, not inferred.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/work_counters.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure;
using namespace seabass::infrastructure::rekordbox;
namespace fs = std::filesystem;

namespace
{

int g_failures = 0;

bool check(bool condition, const std::string &what)
{
    if (!condition) {
        std::cout << "    FAIL: " << what << "\n";
        ++g_failures;
    }
    return condition;
}

std::string verdict(int failuresAtCaseStart)
{
    return g_failures == failuresAtCaseStart ? "OK" : "FAILED";
}

// Every present track row in the database, in the test's own walk of the
// tracks table, so it does not take the index's word for which ids exist.
// Rows with no analysis path are kept: those are the ids the index must
// answer "nothing" for, and they are the ones a lookup could most
// plausibly confuse with a neighbour.
struct TrackRows
{
    // id -> analyze_path as stored, possibly empty. First row wins.
    std::map<uint32_t, std::string> byId;
    size_t rowCount = 0;
};

TrackRows everyTrackRow(const std::string &pioneerRoot)
{
    std::ifstream ifs(pioneerRoot + "/rekordbox/export.pdb", std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("fixture export.pdb not readable");
    }
    kaitai::kstream ks(&ifs);
    rekordbox_pdb_t pdb(false, &ks);

    TrackRows rows;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != rekordbox_pdb_t::PAGE_TYPE_TRACKS) {
            continue;
        }
        forEachDataPage(*table, [&](rekordbox_pdb_t::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    if (auto *track = dynamic_cast<rekordbox_pdb_t::track_row_t *>(row->body())) {
                        ++rows.rowCount;
                        rows.byId.emplace(track->id(), sqlText(track->analyze_path()));
                    }
                }
            }
        });
    }
    return rows;
}

std::string describe(const std::optional<std::string> &path)
{
    return path ? *path : std::string("<nothing>");
}

// What the walk says the index must answer for this row: the stored
// path, or nothing when the row has none.
std::optional<std::string> expectedFor(const std::string &storedPath)
{
    if (storedPath.empty()) {
        return std::nullopt;
    }
    return storedPath;
}

// A pioneerRoot whose rekordbox/export.pdb holds exactly these bytes.
// Checked, because these roots exist to hold bytes the index must
// reject; a write that silently failed would leave a missing or empty
// file, which the index also rejects, and the case would pass without
// ever seeing the bytes it was about.
fs::path rootWithDatabase(const fs::path &scratch, const std::string &name, const std::string &bytes)
{
    const fs::path root = scratch / seabass::pathFromUtf8(name);
    fs::create_directories(root / "rekordbox");
    const fs::path pdb = root / "rekordbox" / "export.pdb";
    {
        std::ofstream out(pdb, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("could not create " + seabass::pathToUtf8(pdb));
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            throw std::runtime_error("could not write " + seabass::pathToUtf8(pdb));
        }
    }
    if (fs::file_size(pdb) != bytes.size()) {
        throw std::runtime_error("short write to " + seabass::pathToUtf8(pdb));
    }
    return root;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// The contract for a database that cannot be read is "the same as the
// single-id lookup": a caller that catches the throw falls back to
// per-id lookups (change_helpers.cpp does), and a database the index
// cannot read must be one the fallback cannot read either, so nothing
// is silently answered from a half-built table.
bool indexThrows(const fs::path &root)
{
    try {
        AnlzPathIndex index(seabass::pathToUtf8(root));
        return false;
    } catch (const std::exception &) {
        return true;
    }
}

bool lookupThrows(const fs::path &root, uint32_t id)
{
    try {
        (void)findAnlzPathForTrackId(seabass::pathToUtf8(root), id);
        return false;
    } catch (const std::exception &) {
        return true;
    }
}

// The index throws or the lookup throws: whichever happened, the other
// must have too. Runs the pair for one unreadable root.
void checkUnreadable(const fs::path &root, uint32_t id, const std::string &what)
{
    check(indexThrows(root), what + ": the index throws instead of coming up empty");
    check(lookupThrows(root, id), what + ": the direct lookup throws on the same bytes");
}

}  // namespace

int main(int argc, char **argv)
{
    const fs::path fixture = argc > 1 ? fs::path(argv[1]) : fs::path("tests/fixtures/anonymized_library");
    const fs::path pioneerRoot = fixture / "rekordbox";
    if (!fs::is_regular_file(pioneerRoot / "rekordbox" / "export.pdb")) {
        std::cerr << "fixture not found at " << fixture << ": run from the repository root\n";
        return 1;
    }
    const std::string root = seabass::pathToUtf8(pioneerRoot);

    const TrackRows rows = everyTrackRow(root);
    const auto &byId = rows.byId;
    if (!check(byId.size() > 100, "the fixture holds a real number of tracks (" + std::to_string(byId.size()) + ")")) {
        return 1;
    }
    // The two walks pick different rows when one id appears twice (the
    // lookup returns at the first present row, the index at the first
    // non-empty path). No writer of this format produces that, and the
    // comparison below assumes it, so say so where it would first fail.
    check(rows.rowCount == byId.size(),
          "every track id appears once (" + std::to_string(rows.rowCount) + " rows, " + std::to_string(byId.size())
              + " ids)");

    // Case 1: one pass. The index's reason to exist is that it parses the
    // database once per save instead of twice per item, so a second parse
    // here would be the regression it was written against.
    int before = g_failures;
    WorkCounters::instance().reset();
    AnlzPathIndex index(root);
    check(WorkCounters::instance().snapshot().trackDatabaseParses == 1,
          "building the index parses export.pdb exactly once");
    size_t withPath = 0;
    for (const auto &[id, path] : byId) {
        withPath += path.empty() ? 0 : 1;
    }
    check(index.size() == withPath, "the index holds one entry per track with a path (" + std::to_string(index.size())
                                        + " entries, " + std::to_string(withPath) + " such tracks)");
    std::cout << "case 1 (one parse, " << index.size() << " paths for " << byId.size() << " tracks) "
              << verdict(before) << "\n";

    // Case 2: every id the database holds resolves through the index to
    // what its own row says: the stored path, or nothing where the row
    // has none. Then the direct lookup, on a spread of those ids, says
    // the same as the index.
    before = g_failures;
    {
        size_t disagreements = 0;
        for (const auto &[id, storedPath] : byId) {
            const auto indexed = index.pathFor(id);
            if (indexed != expectedFor(storedPath)) {
                ++disagreements;
                if (disagreements <= 5) {
                    std::cout << "    id " << id << ": row=" << describe(expectedFor(storedPath))
                              << " index=" << describe(indexed) << "\n";
                }
            }
        }
        check(disagreements == 0, "the index answers every track id with its own row's path ("
                                      + std::to_string(disagreements) + " disagreements)");

        // A spread: first, last, and every 100th in between.
        std::vector<uint32_t> sample;
        size_t position = 0;
        for (const auto &[id, path] : byId) {
            if (position == 0 || position + 1 == byId.size() || position % 100 == 0) {
                sample.push_back(id);
            }
            ++position;
        }
        size_t directDisagreements = 0;
        for (uint32_t id : sample) {
            const auto direct = findAnlzPathForTrackId(root, id);
            if (direct != index.pathFor(id)) {
                ++directDisagreements;
                std::cout << "    id " << id << ": direct=" << describe(direct) << " index="
                          << describe(index.pathFor(id)) << "\n";
            }
        }
        check(directDisagreements == 0, "index and direct lookup agree on " + std::to_string(sample.size())
                                            + " sampled ids (" + std::to_string(directDisagreements)
                                            + " disagreements)");

        // The "row without a path" branch only ran if the fixture has
        // such rows. It is reported, not asserted: the committed fixture
        // has none, and this test cannot make one (PdbRowWriter has no
        // way to blank a path). A regenerated fixture that has one
        // shows up here.
        const size_t withoutPath = byId.size() - withPath;
        std::cout << "case 2 (all " << byId.size() << " ids match their rows, " << withPath << " with a path, "
                  << withoutPath << " without" << (withoutPath == 0 ? " [that branch did not run]" : "") << "; "
                  << sample.size() << " checked against the direct lookup) " << verdict(before) << "\n";
    }

    // Case 3: an id absent from the database yields nothing, not a
    // neighbouring track's path. Every unused id inside the range the
    // fixture spans, plus the ends of the id space. None of these may
    // cost a parse: the header promises every lookup is answered from
    // memory, and a pathFor() that fell back to the direct lookup on a
    // miss would agree on every answer while parsing once per item.
    before = g_failures;
    {
        const auto parsesBefore = WorkCounters::instance().snapshot().trackDatabaseParses;
        size_t absent = 0;
        size_t wrong = 0;
        const uint32_t lowest = byId.begin()->first;
        const uint32_t highest = byId.rbegin()->first;
        for (uint32_t id = lowest; id <= highest; ++id) {
            if (byId.count(id)) {
                continue;
            }
            ++absent;
            if (index.pathFor(id).has_value()) {
                ++wrong;
            }
        }
        check(wrong == 0, "no unused id inside the fixture's range resolves to a path (" + std::to_string(wrong)
                              + " of " + std::to_string(absent) + " did)");
        check(!index.pathFor(0).has_value(), "id 0 resolves to nothing");
        check(!index.pathFor(highest + 1).has_value(), "the id just past the highest resolves to nothing");
        check(!index.pathFor(4294967295u).has_value(), "the largest possible id resolves to nothing");
        check(WorkCounters::instance().snapshot().trackDatabaseParses == parsesBefore,
              "a miss is answered from memory, not by parsing the database again");
        check(!findAnlzPathForTrackId(root, highest + 1).has_value(),
              "the direct lookup agrees that the id just past the highest resolves to nothing");
        std::cout << "case 3 (" << absent << " unused ids in " << lowest << ".." << highest
                  << " resolve to nothing, without a parse) " << verdict(before) << "\n";
    }

    // Cases 4 to 7: a database that cannot be read. The index must not
    // come up empty or partial and let a save proceed against it; it
    // fails the way the single-id lookup fails, so callers take the
    // fallback path and the fallback tells them the same thing.
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_anlz_path_index_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const uint32_t someId = byId.begin()->first;

    before = g_failures;
    {
        const fs::path noDatabase = scratch / "no-database";
        fs::create_directories(noDatabase / "rekordbox");
        checkUnreadable(noDatabase, someId, "missing export.pdb");
        std::cout << "case 4 (missing export.pdb) " << verdict(before) << "\n";
    }

    before = g_failures;
    checkUnreadable(rootWithDatabase(scratch, "garbage", std::string(4096, 'x')), someId,
                    "export.pdb that is not a database");
    std::cout << "case 5 (garbage export.pdb) " << verdict(before) << "\n";

    before = g_failures;
    checkUnreadable(rootWithDatabase(scratch, "empty", std::string()), someId, "empty export.pdb");
    std::cout << "case 6 (empty export.pdb) " << verdict(before) << "\n";

    // Case 7: a truncated database, the shape a stick yanked mid-copy
    // leaves behind. Cut at half, which on this fixture lands inside the
    // tracks table, so both readers hit the cut while walking it. The
    // index and the direct lookup must fail together; and if both do
    // manage to read it, whatever the index answers must be what the
    // direct lookup would answer on the same bytes.
    before = g_failures;
    {
        const std::string whole = readFile(pioneerRoot / "rekordbox" / "export.pdb");
        const fs::path truncated = rootWithDatabase(scratch, "truncated", whole.substr(0, whole.size() / 2));
        const bool threw = indexThrows(truncated);
        const bool lookupThrew = lookupThrows(truncated, someId);
        check(threw == lookupThrew, std::string("on a truncated database the index and the direct lookup fail together (index ")
                                        + (threw ? "threw" : "did not throw") + ", lookup "
                                        + (lookupThrew ? "threw" : "did not throw") + ")");
        size_t answered = 0;
        if (!threw && !lookupThrew) {
            AnlzPathIndex partial(seabass::pathToUtf8(truncated));
            size_t disagreements = 0;
            for (const auto &[id, path] : byId) {
                const auto indexed = partial.pathFor(id);
                answered += indexed ? 1 : 0;
                if (findAnlzPathForTrackId(seabass::pathToUtf8(truncated), id) != indexed) {
                    ++disagreements;
                }
            }
            check(disagreements == 0, "on a truncated database the index never answers what the direct lookup would not ("
                                          + std::to_string(disagreements) + " disagreements)");
        }
        std::cout << "case 7 (truncated export.pdb: "
                  << (threw ? "both throw" : std::to_string(answered) + " paths answered, all as the direct lookup would")
                  << ") " << verdict(before) << "\n";
    }

    fs::remove_all(scratch);

    if (g_failures) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "anlz_path_index_test: all cases passed\n";
    return 0;
}
