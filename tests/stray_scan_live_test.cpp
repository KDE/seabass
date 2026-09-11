// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The unreferenced-file scan against a real stick, with the project's
// own readers, walker, probe and cache -- the run
// docs/unreferenced-file-cleanup-plan.md's step 7 asks for.
//
// A program rather than a checklist, so it can be re-run on the next
// stick and after every change, and so its claims are asserted rather
// than eyeballed. Skips itself (passing) when SEABASS_LIVE_STICK is
// unset, exactly like corpus_test without SEABASS_CORPUS.
//
//   SEABASS_LIVE_STICK=/media/you/RV2 ./stray_scan_live_test
//
// Optional expectations, so a documented stick can be pinned rather than
// merely reported:
//   SEABASS_LIVE_EXPECT_FILES=632         files no catalog references
//   SEABASS_LIVE_EXPECT_DELETABLE=632     of those, proposed for deletion
//   SEABASS_LIVE_EXPECT_SURVIVOR_RULE=9   groups where the survivor rule
//                                         overrode the planner's pick
//   SEABASS_LIVE_SHOW=5                   print that many example groups
//                                         where the survivor rule fired
//
// READS ONLY. It writes exactly one file to the stick, the metadata
// cache (Seabass/caches/metadata.jsonl) the scan itself maintains, and never
// touches a catalog, the pending-deletion manifest, or an audio file.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "application/use_cases/collapse_catalog_rows.hpp"
#include "domain/duplicate_cleanup.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "infrastructure/audio/duration_fill.hpp"
#include "infrastructure/cleanup/stray_file_scan.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

using namespace seabass;
namespace fs = std::filesystem;

namespace
{

int expected(const char *name)
{
    const char *value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::atoi(value) : -1;
}

void checkExpected(const char *name, int actual)
{
    int want = expected(name);
    if (want < 0) {
        return;
    }
    if (want != actual) {
        std::cerr << "FAIL: " << name << " expected " << want << ", got " << actual << "\n";
        std::exit(1);
    }
    std::cout << "  (matches " << name << "=" << want << ")\n";
}

double seconds(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

std::string normalizedPath(std::string path)
{
    for (auto &c : path) {
        if (c == '\\') {
            c = '/';
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return fs::path(path).lexically_normal().generic_string();
}

}  // namespace

int main()
{
    const char *stick = std::getenv("SEABASS_LIVE_STICK");
    if (stick == nullptr || *stick == '\0') {
        // Fail, never skip. This test is only registered when the build
        // was configured with -DSEABASS_LIVE_STICK=<path> (see
        // CMakeLists.txt), which also supplies this variable through
        // ctest -- so reaching here means it is about to check nothing,
        // and returning 0 would report a live scan that never happened.
        std::cerr << "FAIL: SEABASS_LIVE_STICK is unset, so no live stick was scanned.\n"
                     "      Configure with -DSEABASS_LIVE_STICK=/path/to/stick, or run\n"
                     "      this binary with SEABASS_LIVE_STICK set.\n";
        return 1;
    }
    const std::string root = stick;
    std::cout << "stick: " << root << "\n";

    // Every catalog the stick has, read with the real readers. A catalog
    // whose file is there but will not open goes on `failed`, which is
    // what makes the scan refuse rather than under-report.
    application::CatalogTracks catalogs;
    std::vector<std::string> failed;
    // Mirrors LibraryCatalogCache::realScan(), which the GUI reads every
    // catalog through: it fills in missing lengths before anything
    // groups on them. Engine leaves Track.length NULL until it has
    // analyzed a track -- 1214 of 1564 rows on this stick -- and
    // DuplicateTrackFinder never groups a track whose length it does not
    // know, so skipping this step would measure a different library than
    // the app sees. The fill is served from the stick's own duration
    // cache here (no Qt in this test), which is what the app uses too on
    // any stick it has scanned before.
    auto read = [&](const char *name, const std::string &catalogPath, auto &&fn,
                    std::optional<std::vector<domain::Track>> &into) {
        try {
            std::vector<domain::Track> tracks = fn();
            auto filled = infrastructure::audio::fillTrackDurations(tracks, catalogPath);
            into = std::move(tracks);
            std::cout << "  " << name << ": " << into->size() << " rows";
            if (filled.fromCache + filled.probed > 0) {
                std::cout << " (" << filled.fromCache + filled.probed << " lengths filled in, "
                          << filled.unreadable << " still unknown)";
            }
            std::cout << "\n";
        } catch (const std::exception &e) {
            std::cout << "  " << name << ": present but unreadable (" << e.what() << ")\n";
            failed.emplace_back(name);
        }
    };
    if (fs::exists(fs::path(root) / "PIONEER" / "rekordbox" / "export.pdb")) {
        read("rekordbox", root + "/PIONEER",
             [&] { return infrastructure::rekordbox::KaitaiRekordboxReader(root + "/PIONEER").readAll(); },
             catalogs.rekordbox);
    }
    if (fs::exists(fs::path(root) / "Engine Library" / "Database2" / "m.db")) {
        read("engine", root + "/Engine Library",
             [&] { return infrastructure::engine::LibdjinteropEngineReader(root + "/Engine Library").readAll(); },
             catalogs.engine);
    }
    if (fs::exists(fs::path(root) / "PIONEER" / "rekordbox" / "exportLibrary.db")) {
        read("onelibrary", root + "/PIONEER",
             [&] { return infrastructure::onelibrary::OneLibraryReader(root + "/PIONEER").readAll(); },
             catalogs.oneLibrary);
    }

    auto coldStart = std::chrono::steady_clock::now();
    auto scan = infrastructure::cleanup::scanStrayFiles(root, catalogs, failed,
                                                         application::CancellationToken::none());
    double coldSeconds = seconds(coldStart);

    if (!scan.usable) {
        std::cout << "\nscan refused: " << scan.refusal << "\n";
        // A refusal on a stick with an unreadable catalog is the correct
        // answer, not a failure of this run.
        return failed.empty() ? 1 : 0;
    }

    std::string consulted;
    for (const auto &name : scan.catalogsConsulted) {
        consulted += (consulted.empty() ? "" : ", ") + name;
    }
    std::cout << "\nchecked against: " << consulted << "\n";
    std::cout << "unreferenced on disk: " << scan.filesFound << " files, "
              << double(scan.bytesFound) / 1e9 << " GB\n";
    std::cout << "  identified by their tags: " << scan.tracks.size() << ", unreadable: " << scan.unreadable << "\n";
    std::cout << "  walk complete: " << (scan.walkIncomplete ? "NO -- part of the stick unreadable" : "yes") << "\n";
    std::printf("  cold scan: %.1f s\n", coldSeconds);
    checkExpected("SEABASS_LIVE_EXPECT_FILES", static_cast<int>(scan.filesFound));

    // Nothing the catalogs still reference may be in here. This is the
    // one mistake in this feature that cannot be walked back, so it is
    // re-checked against the catalogs independently of the code that
    // produced the list.
    std::set<std::string> referenced;
    for (const auto *catalog : {&catalogs.rekordbox, &catalogs.engine, &catalogs.oneLibrary}) {
        if (!catalog->has_value()) {
            continue;
        }
        for (const auto &t : **catalog) {
            if (!t.filePath.empty()) {
                referenced.insert(normalizedPath(t.filePath));
            }
        }
    }
    for (const auto &t : scan.tracks) {
        if (referenced.count(normalizedPath(t.filePath)) != 0) {
            std::cerr << "FAIL: a catalog still references " << t.filePath << "\n";
            return 1;
        }
    }
    std::cout << "  none of them is referenced by any catalog (re-checked independently)\n";

    // Now the part a page actually runs. The Clean Up page works in ONE
    // catalog at a time -- runRescanTask() groups that format's rows
    // plus the strays -- so that is what is measured here, once per
    // catalog on the stick. The whole-stick pass afterwards answers a
    // different question ("what is on this stick") and is labelled as
    // such rather than as anything a page will show.
    struct Measurement
    {
        int deletable = 0;
        int heldBack = 0;
        int straySurvivors = 0;
        int ungrouped = 0;
        int groupsWithStray = 0;
        int allStrayGroups = 0;
        int survivorRule = 0;
        int ruleNoCataloguedBitrate = 0;
        int ruleRoundedApart = 0;
        std::uint64_t deletableBytes = 0;
    };

    const int show = std::max(0, expected("SEABASS_LIVE_SHOW"));
    int shown = 0;

    auto measure = [&](const std::string &label, const std::vector<domain::Track> &catalogued) -> Measurement {
        Measurement m;
        std::vector<domain::Track> all = catalogued;
        all.insert(all.end(), scan.tracks.begin(), scan.tracks.end());
        std::set<std::string> accountedFor;

        for (const auto &group : domain::DuplicateTrackFinder::find(all)) {
            auto plan = domain::DuplicateCleanupPlanner::plan(group);
            bool hasStray = std::any_of(group.tracks.begin(), group.tracks.end(),
                                         [](const domain::Track &t) { return t.isUnreferenced; });
            if (!hasStray) {
                continue;
            }
            ++m.groupsWithStray;
            bool allStray = std::all_of(group.tracks.begin(), group.tracks.end(),
                                         [](const domain::Track &t) { return t.isUnreferenced; });
            if (allStray) {
                ++m.allStrayGroups;
            }

            // The survivor rule: with any catalogued copy present, the
            // survivor must be one -- otherwise a catalog would be left
            // pointing at a file this proposes to delete.
            if (!allStray && plan.survivor.isUnreferenced) {
                std::cerr << "FAIL (" << label << "): a stray file survived over a catalogued copy: "
                          << plan.survivor.filePath << "\n";
                std::exit(1);
            }
            if (!allStray) {
                // Did it override what the planner would have picked on
                // quality alone? Recomputing that is the only way to
                // count how often the rule actually earns its keep.
                domain::DuplicateGroup unrestricted = group;
                for (auto &t : unrestricted.tracks) {
                    t.isUnreferenced = false;
                }
                const domain::Track wouldHaveWon = domain::DuplicateCleanupPlanner::plan(unrestricted).survivor;
                // Read the flag off the ORIGINAL group: the copy handed
                // to the planner had it cleared, which is the whole
                // trick, so asking the returned Track would always
                // answer "no".
                bool wouldHaveBeenStray = false;
                for (const auto &t : group.tracks) {
                    if (t.format == wouldHaveWon.format && t.sourceId == wouldHaveWon.sourceId) {
                        wouldHaveBeenStray = t.isUnreferenced;
                        break;
                    }
                }
                if (wouldHaveBeenStray) {
                    ++m.survivorRule;
                    // Why it fired, because the two reasons say
                    // different things about the code. Either no
                    // catalogued row in the group knows its own bitrate
                    // (Engine leaves it unset until it analyzes a
                    // track), or one does and the stray still scored
                    // higher -- which for two copies of the same bytes
                    // can only be the probe and the catalog rounding the
                    // same encode differently.
                    int bestCatalogued = 0;
                    for (const auto &t : group.tracks) {
                        if (!t.isUnreferenced) {
                            bestCatalogued = std::max(bestCatalogued, t.bitrate);
                        }
                    }
                    if (bestCatalogued == 0) {
                        ++m.ruleNoCataloguedBitrate;
                    } else {
                        ++m.ruleRoundedApart;
                    }
                    if (shown < show) {
                        ++shown;
                        std::printf("  rule fired (%s): kept %s (%s, %d kbps, %.1f s, %llu bytes)\n", label.c_str(),
                                    plan.survivor.filename.c_str(), plan.survivor.format.c_str(),
                                    plan.survivor.bitrate, plan.survivor.durationSeconds,
                                    (unsigned long long)plan.survivor.fileSizeBytes);
                        std::printf("        over %s (stray, %d kbps, %.1f s, %llu bytes)\n",
                                    wouldHaveWon.filename.c_str(), wouldHaveWon.bitrate,
                                    wouldHaveWon.durationSeconds, (unsigned long long)wouldHaveWon.fileSizeBytes);
                    }
                }
            }
            if (plan.survivor.isUnreferenced) {
                ++m.straySurvivors;
                accountedFor.insert(plan.survivor.filePath);
            }

            for (const auto &t : plan.unreferencedFilesToDelete) {
                if (t.durationIsEstimated) {
                    std::cerr << "FAIL (" << label << "): proposed deleting a file whose length was estimated: "
                              << t.filePath << "\n";
                    std::exit(1);
                }
                ++m.deletable;
                m.deletableBytes += t.fileSizeBytes;
                accountedFor.insert(t.filePath);
            }
            for (const auto &t : plan.unreferencedFilesHeldBack) {
                ++m.heldBack;
                accountedFor.insert(t.filePath);
            }
        }
        m.ungrouped = static_cast<int>(scan.tracks.size()) - static_cast<int>(accountedFor.size());

        std::cout << "\n[" << label << "] groups containing a stray: " << m.groupsWithStray << " ("
                  << m.allStrayGroups << " of them nothing but strays)\n";
        std::cout << "  survivor rule forced a catalogued copy over a better stray: " << m.survivorRule
                  << " groups (" << m.ruleNoCataloguedBitrate << " with no catalogued bitrate at all, "
                  << m.ruleRoundedApart << " rounded apart from the probe)\n";
        std::printf("  proposed for deletion: %d files, %.2f GB\n", m.deletable, double(m.deletableBytes) / 1e9);
        std::cout << "  held back: " << m.heldBack << "; kept as their group's best copy: " << m.straySurvivors
                  << "; matched nothing: " << m.ungrouped << "\n";
        assert(m.deletable + m.heldBack + m.straySurvivors + m.ungrouped == static_cast<int>(scan.tracks.size()));
        return m;
    };

    // One pass per catalog: what that page would actually propose.
    for (const auto *catalog : {&catalogs.rekordbox, &catalogs.engine, &catalogs.oneLibrary}) {
        if (catalog->has_value() && !(*catalog)->empty()) {
            measure((*catalog)->front().format + " page", **catalog);
        }
    }

    // And every catalog at once, collapsed to files first -- what the
    // page is being reworked to do. Without the collapse this pass
    // groups a file with itself, so it also reports how much of that
    // there would be.
    std::vector<domain::Track> everyCatalog;
    for (const auto *catalog : {&catalogs.rekordbox, &catalogs.engine, &catalogs.oneLibrary}) {
        if (catalog->has_value()) {
            everyCatalog.insert(everyCatalog.end(), (*catalog)->begin(), (*catalog)->end());
        }
    }
    auto files = application::collapseCatalogRows(everyCatalog);
    std::cout << "\n" << everyCatalog.size() << " catalog rows collapse to " << files.size() << " files\n";
    int multiCatalog = 0;
    for (const auto &f : files) {
        if (f.catalogRows.size() > 1) {
            ++multiCatalog;
        }
    }
    std::cout << "  " << multiCatalog << " of them are listed by more than one catalog\n";
    Measurement whole = measure("every catalog, collapsed to files", files);

    // Any row for the survivor's own file in a removal list is the
    // mistake the collapse exists to prevent, so it is checked, not
    // assumed.
    for (const auto &group : domain::DuplicateTrackFinder::find(files)) {
        auto plan = domain::DuplicateCleanupPlanner::plan(group);
        for (const auto &doomed : plan.toRemove) {
            for (const auto &r : doomed.catalogRows) {
                for (const auto &s : plan.survivor.catalogRows) {
                    if (r.format == s.format && r.sourceId == s.sourceId) {
                        std::cerr << "FAIL: a row for the surviving file is in its own removal list: " << r.format
                                  << ":" << r.sourceId << "\n";
                        return 1;
                    }
                }
            }
        }
    }
    std::cout << "  no removal list holds a row for the file it keeps\n";

    // Removing a doomed file's row from a catalog repoints that
    // catalog's playlists at the surviving file -- which needs the
    // survivor to HAVE a row there. Where it does not, the honest
    // choices are to hold the group back or to repoint the row at a
    // different file, and how often it happens decides which is
    // affordable.
    int coveredGroups = 0, uncoveredGroups = 0;
    std::map<std::string, int> uncoveredBy;
    for (const auto &group : domain::DuplicateTrackFinder::find(files)) {
        auto plan = domain::DuplicateCleanupPlanner::plan(group);
        if (plan.toRemove.empty()) {
            continue;
        }
        bool covered = true;
        for (const auto &doomed : plan.toRemove) {
            for (const auto &r : doomed.catalogRows) {
                bool survivorHasIt = std::any_of(plan.survivor.catalogRows.begin(),
                                                  plan.survivor.catalogRows.end(),
                                                  [&r](const domain::CatalogRowRef &s) { return s.format == r.format; });
                if (!survivorHasIt) {
                    covered = false;
                    uncoveredBy[r.format] += 1;
                }
            }
        }
        covered ? ++coveredGroups : ++uncoveredGroups;
    }
    std::cout << "\ngroups where the survivor has a row in every catalog the doomed copies do: " << coveredGroups
              << "\n  and where it does not: " << uncoveredGroups << "\n";
    for (const auto &[fmt, n] : uncoveredBy) {
        std::cout << "    " << n << " doomed row(s) in " << fmt << " with no survivor row there\n";
    }
    checkExpected("SEABASS_LIVE_EXPECT_DELETABLE", whole.deletable);
    checkExpected("SEABASS_LIVE_EXPECT_SURVIVOR_RULE", whole.survivorRule);

    // The cache is on the stick, so the second scan is stats rather than
    // tag reads -- and must not change a single answer.
    auto warmStart = std::chrono::steady_clock::now();
    auto again = infrastructure::cleanup::scanStrayFiles(root, catalogs, failed,
                                                          application::CancellationToken::none());
    std::printf("\nwarm scan (cache on the stick): %.1f s, against %.1f s cold\n", seconds(warmStart), coldSeconds);
    if (again.filesFound != scan.filesFound || again.tracks.size() != scan.tracks.size()) {
        std::cerr << "FAIL: the cached scan disagrees with the cold one\n";
        return 1;
    }
    std::cout << "  same answer, file for file\n";

    std::cout << "\nlive stray-file scan OK\n";
    return 0;
}
