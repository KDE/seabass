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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/work_counters.hpp"

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

// Every track id the database holds, in its own walk of the tracks table
// so the test does not take the index's word for which ids exist. Rows
// with no analysis path are kept: those are the ids the index must answer
// "nothing" for, and they are the ones a lookup could most plausibly
// confuse with a neighbour.
std::set<uint32_t> everyTrackId(const std::string &pioneerRoot)
{
    std::ifstream ifs(pioneerRoot + "/rekordbox/export.pdb", std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("fixture export.pdb not readable");
    }
    kaitai::kstream ks(&ifs);
    rekordbox_pdb_t pdb(false, &ks);

    std::set<uint32_t> ids;
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
                        ids.insert(track->id());
                    }
                }
            }
        });
    }
    return ids;
}

// A case's own verdict line: OK only if none of its checks failed.
int g_failuresAtCaseStart = 0;

std::string verdict()
{
    const bool ok = g_failures == g_failuresAtCaseStart;
    g_failuresAtCaseStart = g_failures;
    return ok ? "OK" : "FAILED";
}

std::string describe(const std::optional<std::string> &path)
{
    return path ? *path : std::string("<nothing>");
}

// A pioneerRoot whose rekordbox/export.pdb holds exactly these bytes.
fs::path rootWithDatabase(const fs::path &scratch, const std::string &name, const std::string &bytes)
{
    const fs::path root = scratch / name;
    fs::create_directories(root / "rekordbox");
    std::ofstream out(root / "rekordbox" / "export.pdb", std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return root;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Whether building an index over this root throws, and what the direct
// lookup does for the same root. The contract for a database that cannot
// be read is "the same as the single-id lookup": a caller that catches
// the throw falls back to per-id lookups, and a database the index
// cannot read is one the fallback cannot read either, so nothing is
// silently answered from a half-built table.
bool indexThrows(const fs::path &root)
{
    try {
        AnlzPathIndex index(root.string());
        return false;
    } catch (const std::exception &) {
        return true;
    }
}

bool lookupThrows(const fs::path &root, uint32_t id)
{
    try {
        (void)findAnlzPathForTrackId(root.string(), id);
        return false;
    } catch (const std::exception &) {
        return true;
    }
}

}  // namespace

int main(int argc, char **argv)
{
    const fs::path fixture = argc > 1 ? fs::path(argv[1]) : fs::path("tests/fixtures/anonymized_library");
    const fs::path pioneerRoot = fixture / "rekordbox";
    if (!fs::is_regular_file(pioneerRoot / "rekordbox" / "export.pdb")) {
        std::cerr << "fixture not found at " << fixture << " -- run from the repository root\n";
        return 1;
    }
    const std::string root = pioneerRoot.string();

    const std::set<uint32_t> ids = everyTrackId(root);
    if (!check(ids.size() > 100, "the fixture holds a real number of tracks (" + std::to_string(ids.size()) + ")")) {
        return 1;
    }

    // Case 1: one pass. The index's reason to exist is that it parses the
    // database once per save instead of twice per item, so a second parse
    // here would be the regression it was written against.
    WorkCounters::instance().reset();
    AnlzPathIndex index(root);
    check(WorkCounters::instance().snapshot().trackDatabaseParses == 1,
          "building the index parses export.pdb exactly once");
    check(index.size() > 0 && index.size() <= ids.size(),
          "the index holds at most one entry per track id (" + std::to_string(index.size()) + " of "
              + std::to_string(ids.size()) + ")");
    std::cout << "case 1 (one parse, " << index.size() << " paths for " << ids.size() << " tracks) " << verdict() << "\n";

    // Case 2: every id the database holds resolves identically through
    // the index and through the single-id lookup. Both branches: tracks
    // with a path, and tracks without one, which the index must not
    // answer with a neighbour's.
    {
        size_t withPath = 0;
        size_t withoutPath = 0;
        size_t disagreements = 0;
        for (uint32_t id : ids) {
            const auto direct = findAnlzPathForTrackId(root, id);
            const auto indexed = index.pathFor(id);
            if (direct != indexed) {
                ++disagreements;
                if (disagreements <= 5) {
                    std::cout << "    id " << id << ": direct=" << describe(direct)
                              << " index=" << describe(indexed) << "\n";
                }
            }
            (direct ? withPath : withoutPath) += 1;
        }
        check(disagreements == 0, "index and direct lookup agree on every track id (" + std::to_string(disagreements)
                                      + " disagreements)");
        check(withPath == index.size(),
              "every track the direct lookup resolves is in the index, and nothing else is");
        // The fixture is only evidence for the "no path" branch if it has
        // such rows. Say so rather than pass on a branch that never ran.
        std::cout << "case 2 (agreement on all " << ids.size() << " ids: " << withPath << " with a path, "
                  << withoutPath << " without) " << verdict() << "\n";
    }

    // Case 3: an id absent from the database yields nothing, not a
    // neighbouring track's path. Every unused id inside the range the
    // fixture spans, plus the ends of the id space.
    {
        size_t absent = 0;
        size_t wrong = 0;
        const uint32_t lowest = *ids.begin();
        const uint32_t highest = *ids.rbegin();
        for (uint32_t id = lowest; id <= highest; ++id) {
            if (ids.count(id)) {
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
        check(!findAnlzPathForTrackId(root, highest + 1).has_value(),
              "the direct lookup agrees that the id just past the highest resolves to nothing");
        std::cout << "case 3 (" << absent << " unused ids in " << lowest << ".." << highest << " resolve to nothing) " << verdict() << "\n";
    }

    // Cases 4 to 6: a database that cannot be read. The index must not
    // come up empty or partial and let a save proceed against it; it
    // fails the way the single-id lookup fails, so callers take the
    // fallback path (change_helpers.cpp catches it) and the fallback
    // tells them the same thing.
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_anlz_path_index_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const uint32_t someId = *ids.begin();

    {
        const fs::path root = scratch / "no-database";
        fs::create_directories(root / "rekordbox");
        check(indexThrows(root), "a missing export.pdb throws instead of building an empty index");
        check(lookupThrows(root, someId), "the direct lookup throws on the same missing export.pdb");
        std::cout << "case 4 (missing export.pdb) " << verdict() << "\n";
    }

    {
        const fs::path root = rootWithDatabase(scratch, "garbage", std::string(4096, 'x'));
        check(indexThrows(root), "an export.pdb that is not a database throws instead of building an empty index");
        check(lookupThrows(root, someId), "the direct lookup throws on the same garbage");
        std::cout << "case 5 (garbage export.pdb) " << verdict() << "\n";
    }

    {
        const fs::path root = rootWithDatabase(scratch, "empty", std::string());
        check(indexThrows(root), "an empty export.pdb throws instead of building an empty index");
        check(lookupThrows(root, someId), "the direct lookup throws on the same empty file");
        std::cout << "case 6 (empty export.pdb) " << verdict() << "\n";
    }

    // Case 7: a truncated database, the shape a stick yanked mid-copy
    // leaves behind. Whatever the index manages to answer must be what
    // the direct lookup would answer on the same bytes; and where the
    // direct lookup cannot read the file, neither may the index.
    {
        const std::string whole = readFile(pioneerRoot / "rekordbox" / "export.pdb");
        const fs::path root = rootWithDatabase(scratch, "truncated", whole.substr(0, whole.size() / 2));
        const bool threw = indexThrows(root);
        size_t answered = 0;
        size_t disagreements = 0;
        if (!threw) {
            AnlzPathIndex partial(root.string());
            for (uint32_t id : ids) {
                std::optional<std::string> direct;
                bool directThrew = false;
                try {
                    direct = findAnlzPathForTrackId(root.string(), id);
                } catch (const std::exception &) {
                    directThrew = true;
                }
                const auto indexed = partial.pathFor(id);
                if (indexed) {
                    ++answered;
                }
                if (directThrew ? indexed.has_value() : direct != indexed) {
                    ++disagreements;
                }
            }
        }
        check(disagreements == 0,
              "on a truncated database the index never answers what the direct lookup would not ("
                  + std::to_string(disagreements) + " disagreements)");
        std::cout << "case 7 (truncated export.pdb: " << (threw ? "throws" : std::to_string(answered) + " paths answered")
                  << ", agrees with the direct lookup) " << verdict() << "\n";
    }

    fs::remove_all(scratch);

    if (g_failures) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "anlz_path_index_test: all cases passed\n";
    return 0;
}
