// Runs every library in the corpus through the checks that matter:
// integrity, stability, and how much work the code does per item.
//
// The corpus is the committed anonymized fixture plus, when
// SEABASS_CORPUS names a directory, every set inside it. Those extra sets
// are real, un-anonymized libraries extracted by tools/extract_testdata
// and they never leave the machine, which is exactly why they are the
// ones that find things: anonymized titles and artists are placeholders,
// so they cannot exercise the fuzzy matching that Sync and Duplicates are
// built on. See docs/real-data-testing.md.
//
// Everything runs from local disk. Timing is deliberately NOT asserted
// here: it is a property of the medium, not the code (the same Engine
// write is 151 ms on a stick and 0.8 ms on a ramdisk), so this asserts
// operation COUNTS, which are the same number everywhere. Wall clock
// lives in tools/stick_write_bench against real hardware.
//
// Every write happens on a scratch copy. No set is ever modified.
//
// Counts that used to be hardcoded to the committed fixture (1370 tracks,
// 188 cues, and so on) are now recorded per set in SET-EXPECTATIONS.txt
// beside the set, written on first sight and asserted afterwards. That
// keeps the fixture's exact regression guard while giving every other
// library the same one, which is what catches a reader regression against
// data nobody has looked at by hand.

#include <set>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/collapse_catalog_rows.hpp"
#include "application/use_cases/scan_library.hpp"
#include "application/use_cases/sync_libraries.hpp"
#include "domain/library_consistency.hpp"
#include "domain/library_statistics.hpp"
#include "domain/track_scope.hpp"
#include "application/use_cases/find_unreferenced_files.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/cleanup/pending_deletion_applier.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "infrastructure/cleanup/pending_deletion_resolver.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_cleanup_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "infrastructure/work_counters.hpp"
#include "infrastructure/rekordbox/rekordbox_settings_fields.hpp"
#include "infrastructure/rekordbox/rekordbox_settings_reader.hpp"
#include "infrastructure/zip_archive_reader.hpp"

#ifdef SEABASS_CORPUS_HAS_EDIT
#include <QString>

#include <memory>

#include "domain/duplicate_cleanup.hpp"
#include "domain/local_restore.hpp"
#include "domain/sync_planning.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "gui/edit/changes/cleanup_group_change.hpp"
#include "gui/edit/changes/copy_cues_change.hpp"
#include "gui/edit/changes/delete_orphan_change.hpp"
#include "gui/edit/changes/merge_cues_change.hpp"
#include "gui/edit/changes/repair_issue_change.hpp"
#include "gui/edit/changes/device_setting_change.hpp"
#include "gui/edit/changes/remove_junk_cue_change.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"

#include "scratch_path.hpp"
#endif

namespace fs = std::filesystem;
using namespace seabass;
using infrastructure::WorkCounters;

namespace
{

int g_failures = 0;
std::string g_set;

bool check(bool condition, const std::string &what)
{
    if (!condition) {
        std::cout << "    FAIL [" << g_set << "]: " << what << "\n";
        ++g_failures;
    }
    return condition;
}

void pass(const std::string &what)
{
    std::cout << "    ok: " << what << "\n";
}

// ------------------------------------------------------------- data sets

// A set is a directory holding one or both catalogs. Two layouts exist and
// both are legitimate: the anonymizer writes "rekordbox/" and "engine/",
// while tools/extract_testdata mirrors a stick's own "PIONEER/" and
// "Engine Library/". Which layout it is also says whether the content is
// anonymized, which decides whether the placeholder-shape cases apply.
struct DataSet
{
    std::string name;
    fs::path root;
    // Where this set's recorded numbers live. Normally inside the set, but
    // a zipped set is unpacked into a scratch directory that is deleted at
    // the end of the run, so its file has to live beside the archive
    // instead -- otherwise every run re-records and the guard never fires.
    fs::path expectationsPath;
    std::optional<std::string> rekordboxRoot;
    std::optional<std::string> engineRoot;
    bool anonymized = false;
};

std::optional<std::string> firstExisting(const fs::path &base, std::initializer_list<const char *> candidates)
{
    std::error_code ec;
    for (const char *candidate : candidates) {
        const fs::path path = base / candidate;
        if (fs::is_directory(path, ec)) {
            return path.string();
        }
    }
    return std::nullopt;
}

std::optional<DataSet> asDataSet(const fs::path &dir)
{
    DataSet set;
    set.name = dir.filename().string();
    set.root = dir;
    set.expectationsPath = dir / "SET-EXPECTATIONS.txt";
    std::error_code ec;

    // Pick ONE layout and take both catalogs from it. A set can hold both
    // -- an early collected archive carries raw PIONEER/ and Engine
    // Library/ copies beside the anonymized tree -- and taking the
    // rekordbox half from one and the Engine half from the other compares
    // placeholders against real titles, which matches nothing and looks
    // exactly like a broken matcher. That cost an hour once; it does not
    // get to happen twice.
    const bool hasAnonymized = fs::is_directory(dir / "rekordbox", ec) || fs::is_directory(dir / "engine", ec);
    const bool hasRaw = fs::is_directory(dir / "PIONEER", ec) || fs::is_directory(dir / "Engine Library", ec);
    if (hasAnonymized && hasRaw) {
        // Worth saying out loud rather than quietly preferring one: an
        // archive meant to be shareable that also carries raw copies is a
        // privacy problem, not just an awkward layout.
        std::cout << "  NOTE: " << set.name
                  << " holds an anonymized tree AND raw stick copies. Reading the anonymized one.\n"
                     "        An export meant for sharing should not contain the raw copies at all.\n";
    }
    set.anonymized = hasAnonymized;
    if (hasAnonymized) {
        set.rekordboxRoot = firstExisting(dir, {"rekordbox"});
        set.engineRoot = firstExisting(dir, {"engine"});
    } else {
        set.rekordboxRoot = firstExisting(dir, {"PIONEER"});
        set.engineRoot = firstExisting(dir, {"Engine Library"});
    }
    if (!set.rekordboxRoot && !set.engineRoot) {
        return std::nullopt;
    }
    return set;
}

// Where a zipped set gets unpacked, once, before anything reads it.
fs::path unpackedSetsRoot()
{
    return seabass::testing::scratchRoot() / "seabass_corpus_unpacked";
}

// A set kept as one file rather than six thousand. Unpacked into a scratch
// directory and then treated exactly like a directory set.
std::optional<DataSet> unpackZippedSet(const fs::path &zipPath)
{
    const std::string stem = zipPath.stem().string();
    const fs::path target = unpackedSetsRoot() / stem;
    std::error_code ec;
    fs::remove_all(target, ec);
    try {
        infrastructure::extractZipArchive(zipPath, target);
    } catch (const std::exception &e) {
        std::cout << "skipping " << zipPath.filename().string() << ": could not unpack it (" << e.what() << ")\n";
        return std::nullopt;
    }
    auto set = asDataSet(target);
    if (!set) {
        std::cout << "skipping " << zipPath.filename().string()
                  << ": unpacked, but holds neither catalog (no rekordbox/PIONEER, no engine/Engine Library)\n";
        fs::remove_all(target, ec);
        return std::nullopt;
    }
    set->name = stem;
    set->expectationsPath = zipPath.parent_path() / (stem + "-EXPECTATIONS.txt");
    std::cout << "unpacked " << zipPath.filename().string() << " into " << target.string() << "\n";
    return set;
}

std::vector<DataSet> discoverSets()
{
    std::vector<DataSet> sets;
    if (auto committed = asDataSet("tests/fixtures/anonymized_library")) {
        committed->name = "committed fixture";
        sets.push_back(*committed);
    } else {
        std::cout << "WARNING: the committed fixture was not found -- run from the repository root\n";
        ++g_failures;
    }
    const char *corpus = std::getenv("SEABASS_CORPUS");
    if (corpus == nullptr || *corpus == '\0') {
        std::cout << "SEABASS_CORPUS is not set: running against the committed fixture alone.\n"
                     "Matching cases need real titles and artists, so they will be skipped.\n";
        return sets;
    }
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(corpus, ec)) {
        if (!entry.is_directory()) {
            if (entry.path().extension() == ".zip") {
                if (auto set = unpackZippedSet(entry.path())) {
                    sets.push_back(*set);
                }
                continue;
            }
            // The runner writes a zipped set's numbers beside the
            // archive, so those are its own files, not candidates.
            if (entry.path().filename().string().find("-EXPECTATIONS.txt") == std::string::npos) {
                std::cout << "skipping " << entry.path().filename().string()
                          << ": neither a directory nor a .zip\n";
            }
            continue;
        }
        if (auto set = asDataSet(entry.path())) {
            sets.push_back(*set);
        } else {
            std::cout << "skipping " << entry.path().filename().string()
                      << ": holds neither catalog (no rekordbox/PIONEER, no engine/Engine Library)\n";
        }
    }
    return sets;
}

// Case 7 works from one catalog by construction: it removes a row from the
// rekordbox export and checks the resolver against a fresh read of that
// same catalog. resolvePendingDeletions() takes them named rather than
// merged so that "I only passed one catalog" cannot be invisible at the
// call site -- here it genuinely is one, and this says so.
application::CatalogTracks asRekordboxCatalog(std::vector<domain::Track> tracks)
{
    application::CatalogTracks catalogs;
    catalogs.rekordbox = std::move(tracks);
    return catalogs;
}

// ---------------------------------------------------------- expectations

// Numbers a set is expected to keep producing. Recorded the first time a
// set is seen, asserted every time after. A deliberate change (a
// regenerated fixture, a set re-extracted from a stick that has moved on)
// means deleting the file and letting the next run record it again.
class Expectations
{
public:
    explicit Expectations(fs::path path) : m_path(std::move(path))
    {
        std::ifstream in(m_path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::istringstream parsed(line);
            std::string key;
            long long value = 0;
            if (parsed >> key >> value) {
                m_values[key] = value;
            }
        }
    }

    // Returns true when the value matched what was recorded, or was
    // recorded now for the first time.
    bool expect(const std::string &key, long long actual, const std::string &what)
    {
        auto it = m_values.find(key);
        if (it == m_values.end()) {
            m_values[key] = actual;
            m_dirty = true;
            std::cout << "    recorded " << key << " = " << actual << "\n";
            return true;
        }
        return check(it->second == actual,
                     what + " (" + key + " was " + std::to_string(it->second) + ", now " + std::to_string(actual) + ")");
    }

    // For a value that may legitimately improve but must never regress.
    bool expectAtLeast(const std::string &key, long long actual, const std::string &what)
    {
        auto it = m_values.find(key);
        if (it == m_values.end()) {
            m_values[key] = actual;
            m_dirty = true;
            std::cout << "    recorded " << key << " = " << actual << "\n";
            return true;
        }
        if (actual > it->second) {
            std::cout << "    " << key << " improved on " << it->second << " (now " << actual
                      << "); update the file when ready\n";
            return true;
        }
        return check(actual >= it->second,
                     what + " (" + key + " was " + std::to_string(it->second) + ", now " + std::to_string(actual) + ")");
    }

    void save()
    {
        if (!m_dirty) {
            return;
        }
        // Written through a temp file and renamed, because this one path
        // is shared by every corpus_test process on the machine and two
        // of them re-recording at once would otherwise interleave into a
        // half-written file that neither run could read back. Rare -- it
        // needs two runs recording new expectations at the same moment --
        // and cheap enough not to reason about.
        const fs::path temp = m_path.string() + ".tmp-" + std::to_string(::getpid());
        {
            std::ofstream out(temp, std::ios::trunc);
            out << "# Written by corpus_test. Delete this file to re-record after a\n"
                   "# deliberate change to the data set itself.\n";
            for (const auto &[key, value] : m_values) {
                out << key << " " << value << "\n";
            }
        }
        std::error_code ec;
        fs::rename(temp, m_path, ec);
        if (ec) {
            fs::remove(temp, ec);
        }
    }

private:
    fs::path m_path;
    std::map<std::string, long long> m_values;
    bool m_dirty = false;
};

// ---------------------------------------------------------------- shared

struct Catalogs
{
    std::vector<domain::Track> rekordbox;
    std::vector<domain::Track> engine;
    // The Device Library Plus mirror that lives beside export.pdb. Real
    // sticks have one; the anonymized fixture does not, because that
    // database has no anonymizer yet.
    std::vector<domain::Track> oneLibrary;
};

fs::path scratchFor(const std::string &name)
{
    std::string safe;
    for (char c : name) {
        safe += (c == ' ' || c == '/') ? '_' : c;
    }
    fs::path root = seabass::testing::scratchRoot() / ("seabass_corpus_test_" + safe);
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

// A writable copy of the set's rekordbox tree. Several cases mutate one,
// and each needs to start from the untouched original.
fs::path freshRekordboxCopy(const DataSet &set, const fs::path &scratch, const std::string &name)
{
    fs::path target = scratch / name;
    fs::remove_all(target);
    fs::copy(*set.rekordboxRoot, target, fs::copy_options::recursive);
    return target;
}

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Tracks whose catalog recorded an album. A guarded count rather than a
// spot check: album is resolved through a normalized table in all three
// formats, so a reader that silently stops resolving it reports every
// track as album-less while everything else still passes.
long long countWithAlbum(const std::vector<domain::Track> &tracks)
{
    return std::count_if(tracks.begin(), tracks.end(),
                          [](const domain::Track &t) { return !t.album.empty(); });
}

int countCues(const std::vector<domain::Track> &tracks)
{
    int total = 0;
    for (const auto &t : tracks) {
        total += static_cast<int>(t.cues.size());
    }
    return total;
}

int countWithCues(const std::vector<domain::Track> &tracks)
{
    int total = 0;
    for (const auto &t : tracks) {
        if (!t.cues.empty()) {
            ++total;
        }
    }
    return total;
}

// ------------------------------------------------------ integrity cases

// Case 1 and 2: both readers return the same library they returned last
// time. Exact numbers rather than "more than zero", because a reader that
// silently drops a row on data nobody inspects by hand is precisely the
// regression this corpus exists to catch.
void caseScanCounts(const DataSet &set, Catalogs &catalogs, Expectations &expected)
{
    if (set.rekordboxRoot) {
        try {
            infrastructure::rekordbox::KaitaiRekordboxReader reader(*set.rekordboxRoot);
            catalogs.rekordbox = application::ScanLibrary(reader).execute();
        } catch (const std::exception &e) {
            check(false, std::string("the rekordbox catalog could not be read at all: ") + e.what());
        }
        if (check(!catalogs.rekordbox.empty(), "rekordbox scan returned tracks")) {
            expected.expect("rekordbox.tracks", static_cast<long long>(catalogs.rekordbox.size()),
                            "rekordbox track count unchanged");
            expected.expect("rekordbox.tracksWithCues", countWithCues(catalogs.rekordbox),
                            "rekordbox tracks-with-cues unchanged");
            expected.expect("rekordbox.cues", countCues(catalogs.rekordbox), "rekordbox cue count unchanged");
            expected.expect("rekordbox.tracksWithAlbum", countWithAlbum(catalogs.rekordbox),
                            "rekordbox tracks-with-album unchanged");
            pass("case 1: rekordbox scan at real scale, track and cue counts hold");
        }
    }
    if (set.rekordboxRoot && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(*set.rekordboxRoot)) {
        try {
            infrastructure::onelibrary::OneLibraryReader reader(*set.rekordboxRoot);
            catalogs.oneLibrary = reader.readAll();
            expected.expect("onelibrary.tracks", static_cast<long long>(catalogs.oneLibrary.size()),
                            "OneLibrary track count unchanged");
            pass("case 2b: the OneLibrary mirror reads at real scale");
        } catch (const std::exception &e) {
            check(false, std::string("the OneLibrary mirror could not be read: ") + e.what());
        }
    }
    if (set.engineRoot) {
        try {
            infrastructure::engine::LibdjinteropEngineReader reader(*set.engineRoot);
            catalogs.engine = application::ScanLibrary(reader).execute();
        } catch (const std::exception &e) {
            check(false, std::string("the Engine catalog could not be read at all: ") + e.what());
        }
        if (check(!catalogs.engine.empty(), "Engine scan returned tracks")) {
            expected.expect("engine.tracks", static_cast<long long>(catalogs.engine.size()),
                            "Engine track count unchanged");
            expected.expect("engine.tracksWithCues", countWithCues(catalogs.engine),
                            "Engine tracks-with-cues unchanged");
            expected.expect("engine.cues", countCues(catalogs.engine), "Engine cue count unchanged");
            expected.expect("engine.tracksWithAlbum", countWithAlbum(catalogs.engine),
                            "Engine tracks-with-album unchanged");
            pass("case 2: Engine scan at real scale, track and cue counts hold");
        }
    }
}

// Case 3: the statistics a page draws are non-degenerate on a real
// library. A calculator that returns an empty distribution renders an
// empty page, which no unit test on four synthetic tracks would notice.
void caseStatistics(const Catalogs &catalogs)
{
    if (catalogs.rekordbox.empty()) {
        return;
    }
    auto stats = domain::LibraryStatisticsCalculator::calculate(catalogs.rekordbox);
    check(stats.trackCount == static_cast<int>(catalogs.rekordbox.size()), "statistics counted every track");
    check(stats.playlistCount > 0, "statistics found playlists");
    check(!stats.bpmDistribution.empty(), "statistics produced a BPM distribution");
    check(!stats.tracksPerKey.empty(), "statistics produced a key distribution");
    check(!stats.tracksPerFileFormat.empty(), "statistics produced a file-format distribution");
    pass("case 3: statistics are sane and non-degenerate at real scale");
}

// Case 4: the two catalogs describe the same stick, so matching should
// pair nearly every track. This is the case the raw sets exist for: on an
// anonymized set the matcher runs on placeholder strings, whose fuzziness
// is nothing like real punctuation, accents, "feat." spellings and remix
// suffixes.
void caseSyncMatching(const DataSet &set, const Catalogs &catalogs, Expectations &expected)
{
    if (catalogs.rekordbox.empty() || catalogs.engine.empty()) {
        std::cout << "    skipped case 4 (matching): this set has only one catalog\n";
        return;
    }
    auto now = std::chrono::system_clock::now();
    auto plans = application::SyncLibraries().execute(catalogs.rekordbox, catalogs.engine, now, now);
    const auto matched = static_cast<long long>(plans.size());

    if (set.anonymized) {
        // The property the anonymized fixture exists to prove: the two
        // catalogs were anonymized in two independent runs and must still
        // pair up. A floor rather than equality so an incidental hash
        // collision does not fail a freshly generated fixture.
        check(matched >= static_cast<long long>(catalogs.rekordbox.size() * 0.95),
              "at least 95% of tracks matched across independently anonymized catalogs");
    }
    expected.expectAtLeast("sync.matched", matched, "matching did not get worse");
    std::cout << "    matched " << matched << " of " << catalogs.rekordbox.size() << "\n";
    pass(set.anonymized ? "case 4: matching holds across independently anonymized catalogs"
                        : "case 4: matching holds on real titles and artists");
}

// Case 4b: no two different real tracks collapsed onto the same
// placeholder. Guards a real bug: the placeholder used to put its
// human-readable label before its hash, and the writer preserves the
// original field's byte length, so short titles lost the hash entirely
// and 7.6% of a real library collapsed onto a handful of strings.
// Anonymized sets only -- a raw library legitimately holds duplicates.
void casePlaceholderCollisions(const DataSet &set, const Catalogs &catalogs)
{
    if (!set.anonymized) {
        std::cout << "    skipped case 4b (placeholder collisions): this set is not anonymized\n";
        return;
    }
    auto collisions = [](const std::vector<domain::Track> &tracks) {
        std::map<std::pair<std::string, std::string>, int> byTitleArtist;
        std::map<std::string, int> byFilename;
        for (const auto &t : tracks) {
            ++byTitleArtist[{t.title, t.artist}];
            ++byFilename[t.filename];
        }
        int titleArtist = 0;
        int filename = 0;
        for (const auto &[k, v] : byTitleArtist) {
            if (v > 1) {
                titleArtist += v;
            }
        }
        for (const auto &[k, v] : byFilename) {
            if (v > 1) {
                filename += v;
            }
        }
        return std::make_pair(titleArtist, filename);
    };
    if (!catalogs.rekordbox.empty()) {
        auto [titleArtist, filename] = collisions(catalogs.rekordbox);
        check(titleArtist == 0, "no two rekordbox tracks share an anonymized title and artist");
        check(filename == 0, "no two rekordbox tracks share an anonymized filename");
    }
    if (!catalogs.engine.empty()) {
        auto [titleArtist, filename] = collisions(catalogs.engine);
        check(titleArtist == 0, "no two Engine tracks share an anonymized title and artist");
        check(filename == 0, "no two Engine tracks share an anonymized filename");
    }
    pass("case 4b: every anonymized track is still distinguishable from every other");
}

// Case 5: the consistency checker classifies a real library without
// falling over. Anonymized paths point at files that do not exist, so
// every row is "broken" there; a raw set's files may genuinely be
// present, so the count is reported, not asserted to be non-zero.
void caseConsistencyChecker(const Catalogs &catalogs)
{
    if (catalogs.rekordbox.empty()) {
        return;
    }
    std::vector<domain::LibraryConsistencyIssue> issues;
    try {
        issues = domain::LibraryConsistencyChecker::check({}, catalogs.rekordbox);
    } catch (const std::exception &e) {
        check(false, std::string("the consistency checker threw at real scale: ") + e.what());
        return;
    }
    std::cout << "    classified " << issues.size() << " issue(s)\n";
    pass("case 5: the consistency checker survives a real library");
}

// Case 6: a cue written to a real library reads back as the same cue.
// Reading back with a fresh reader is the whole point -- every write-path
// bug this project has found was invisible to a test that trusted its own
// return value.
void caseCueRoundTrip(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs)
{
    if (catalogs.rekordbox.empty()) {
        return;
    }
    const domain::Track *target = nullptr;
    for (const auto &track : catalogs.rekordbox) {
        if (track.cues.empty()) {
            target = &track;
            break;
        }
    }
    if (target == nullptr) {
        std::cout << "    skipped case 6 (round trip): every track already has cues\n";
        return;
    }
    const std::string targetId = target->sourceId;
    const fs::path root = freshRekordboxCopy(set, scratch, "roundtrip");

    domain::CuePoint cue;
    cue.kind = domain::CuePoint::Kind::Hot;
    cue.hotCueNumber = 1;
    cue.positionMs = 12345.0;
    cue.color = "#FF0000";

    infrastructure::rekordbox::RekordboxCueWriter writer(root.string());
    writer.writeHotCues(targetId, {cue});

    infrastructure::rekordbox::KaitaiRekordboxReader rereader(root.string());
    auto reread = application::ScanLibrary(rereader).execute();
    bool found = false;
    for (const auto &track : reread) {
        if (track.sourceId != targetId) {
            continue;
        }
        found = true;
        if (check(track.cues.size() == 1, "exactly the written cue came back")) {
            check(track.cues[0].kind == domain::CuePoint::Kind::Hot, "cue kind survived");
            check(track.cues[0].hotCueNumber == 1, "hot cue number survived");
            check(track.cues[0].positionMs == 12345.0, "cue position survived");
        }
    }
    check(found, "the written track is still in the library");
    fs::remove_all(root);
    pass("case 6: a real hot-cue write round-trips at realistic file scale");
}

// Case 7: the orphaned-file deletion chain end to end -- a real cleanup
// pass, a real manifest entry, the stale-manifest refusal, then the one
// operation in this app that destroys real audio.
void casePendingDeletion(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs)
{
    if (catalogs.rekordbox.size() < 30) {
        std::cout << "    skipped case 7 (deletion chain): too few rekordbox tracks\n";
        return;
    }
    const fs::path root = freshRekordboxCopy(set, scratch, "deletion");
    infrastructure::rekordbox::KaitaiRekordboxReader reader(root.string());
    auto tracks = application::ScanLibrary(reader).execute();
    const size_t before = tracks.size();

    domain::Track doomed = tracks[10];
    domain::Track survivor = tracks[20];
    if (!check(doomed.sourceId != survivor.sourceId, "picked two distinct tracks")) {
        return;
    }
    // A real set's audio may actually exist and must never be deleted, so
    // this case writes its own stand-in file inside the scratch tree and
    // points the manifest at that instead of the track's real path.
    const fs::path victim = scratch / "deletion-victim.mp3";
    std::ofstream(victim) << "fake audio data";
    doomed.filePath = victim.string();

    fs::path manifestPath = scratch / "Seabass" / "orphaned" / "pending-deletions.jsonl";
    fs::remove(manifestPath);
    infrastructure::cleanup::PendingDeletionManifest manifest(manifestPath.string());

    infrastructure::rekordbox::RekordboxCleanupWriter cleanupWriter(root.string());
    cleanupWriter.removeTrackReplacingWith(doomed.sourceId, survivor.sourceId);

    infrastructure::cleanup::PendingDeletion pending;
    pending.format = "rekordbox";
    pending.filePath = doomed.filePath;
    pending.title = doomed.title;
    pending.artist = doomed.artist;
    pending.backupId = "corpus-test";
    manifest.append(pending);

    infrastructure::rekordbox::KaitaiRekordboxReader postRemovalReader(root.string());
    auto postRemoval = application::ScanLibrary(postRemovalReader).execute();
    check(postRemoval.size() == before - 1, "the removed row is gone from a fresh scan");
    bool stillThere = false;
    for (const auto &t : postRemoval) {
        if (t.sourceId == doomed.sourceId) {
            stillThere = true;
        }
    }
    check(!stillThere, "the removed row is really gone, not just uncounted");
    pass("case 7a: a real cleanup pass removes the row and records the pending deletion");

    // Stale manifest: some other track now legitimately occupies that
    // path. The resolver must refuse the deletion even though the
    // manifest still lists it.
    auto staleScan = postRemoval;
    staleScan[0].filePath = doomed.filePath;
    auto stale = infrastructure::cleanup::resolvePendingDeletions(manifest.list(), asRekordboxCatalog(staleScan), scratch.string());
    check(stale.safeToDelete.empty(), "a still-referenced path is not offered for deletion");
    if (check(stale.stillReferenced.size() == 1, "the still-referenced path is reported as such")) {
        check(stale.stillReferenced[0].filePath == doomed.filePath, "the right path was protected");
    }
    check(fs::exists(victim), "the protected file is still on disk");
    pass("case 7b: a stale manifest entry is refused, not acted on");

    auto real = infrastructure::cleanup::resolvePendingDeletions(manifest.list(), asRekordboxCatalog(postRemoval), scratch.string());
    if (check(real.safeToDelete.size() == 1, "the genuinely orphaned file is offered for deletion")) {
        check(real.stillReferenced.empty(), "nothing else was flagged");
        auto outcomes = infrastructure::cleanup::applyPendingDeletions(real.safeToDelete, scratch.string(), manifest);
        if (check(outcomes.size() == 1, "one deletion was attempted")) {
            check(outcomes[0].status == infrastructure::cleanup::PendingDeletionOutcome::Status::Deleted,
                  "the deletion reported success");
        }
        check(!fs::exists(victim), "the orphaned file is genuinely gone from disk");
        check(manifest.list().empty(), "the manifest was cleared");
    }
    fs::remove_all(root);
    pass("case 7c: a genuinely orphaned file is deleted and the manifest cleared");
}

// Case 8: a batch interrupted partway is fully revertible from the one
// backup taken before it started, including real playlist membership --
// not just the bare row.
void caseInterruptedBatch(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs)
{
    if (catalogs.rekordbox.size() < 30) {
        std::cout << "    skipped case 8 (rollback): too few rekordbox tracks\n";
        return;
    }
    const fs::path root = freshRekordboxCopy(set, scratch, "rollback");
    infrastructure::rekordbox::KaitaiRekordboxReader reader(root.string());
    auto tracks = application::ScanLibrary(reader).execute();
    const size_t before = tracks.size();

    // A track with real playlist membership, so the restore genuinely has
    // playlist repointing to undo.
    const domain::Track *doomed = nullptr;
    for (const auto &t : tracks) {
        if (!t.playlists.empty()) {
            doomed = &t;
            break;
        }
    }
    if (doomed == nullptr) {
        std::cout << "    skipped case 8 (rollback): no track has playlist membership\n";
        fs::remove_all(root);
        return;
    }
    const std::string doomedId = doomed->sourceId;
    const std::string doomedTitle = doomed->title;
    const auto playlistsBefore = doomed->playlists;

    const domain::Track *survivor = nullptr;
    for (const auto &t : tracks) {
        if (t.sourceId != doomedId) {
            survivor = &t;
            break;
        }
    }
    if (!check(survivor != nullptr, "found a survivor for the rollback case")) {
        return;
    }

    // Mirrors the save loop's own rule: export.pdb is the one shared file
    // every group's write touches, backed up exactly once before any of
    // them run.
    infrastructure::backup::FilesystemBackupStore backupStore((scratch / "rollback-backups").string());
    const std::string pdbPath = (root / "rekordbox" / "export.pdb").string();
    if (!check(fs::exists(pdbPath), "the catalog database is where the backup expects it")) {
        return;
    }
    auto record = backupStore.backup({pdbPath}, "duplicate-file-cleanup");

    {
        infrastructure::rekordbox::RekordboxCleanupWriter cleanupWriter(root.string());
        cleanupWriter.removeTrackReplacingWith(doomedId, survivor->sourceId);
    }

    {
        infrastructure::rekordbox::KaitaiRekordboxReader midReader(root.string());
        auto mid = application::ScanLibrary(midReader).execute();
        check(mid.size() == before - 1, "the first group's removal landed");
        bool present = false;
        for (const auto &t : mid) {
            if (t.sourceId == doomedId) {
                present = true;
            }
        }
        check(!present, "the first group's doomed row is gone");
        pass("case 8a: an interrupted batch leaves exactly the groups it finished");
    }

    check(backupStore.restore(record.id), "the backup restored");

    {
        infrastructure::rekordbox::KaitaiRekordboxReader postReader(root.string());
        auto post = application::ScanLibrary(postReader).execute();
        check(post.size() == before, "every row is back");
        const domain::Track *restored = nullptr;
        for (const auto &t : post) {
            if (t.sourceId == doomedId) {
                restored = &t;
            }
        }
        if (check(restored != nullptr, "the removed row came back")) {
            check(restored->title == doomedTitle, "it came back with its title");
            if (check(restored->playlists.size() == playlistsBefore.size(),
                      "it came back with all its playlist entries")) {
                for (size_t i = 0; i < playlistsBefore.size(); ++i) {
                    check(restored->playlists[i].name == playlistsBefore[i].name, "playlist name survived the restore");
                    check(restored->playlists[i].position == playlistsBefore[i].position,
                          "playlist position survived the restore");
                }
            }
        }
        pass("case 8b: restoring the one upfront backup reverts the batch completely");
    }
    fs::remove_all(root);
}

// ------------------------------------------------------- stability cases

// Real libraries contain data libdjinterop cannot decode. The contract is
// not "never refuses" -- it is "refuses visibly, one track at a time, and
// no more often than last time".
void caseEngineStability(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs, int sampleSize,
                         Expectations &expected)
{
    if (catalogs.engine.empty()) {
        return;
    }
    const fs::path root = scratch / "engine-stability";
    fs::remove_all(root);
    fs::copy(*set.engineRoot, root, fs::copy_options::recursive);

    int attempted = 0;
    int refused = 0;
    // Scoped so the writer's own SQLite connection is closed before
    // remove_all() below deletes the tree -- POSIX happily unlinks a file
    // still open elsewhere in the same process (frees it on last close),
    // so this only showed up on Windows, which locks it.
    {
        infrastructure::engine::LibdjinteropEngineCueWriter writer(root.string());
        domain::CuePoint cue;
        cue.kind = domain::CuePoint::Kind::Hot;
        cue.hotCueNumber = 1;
        cue.positionMs = 1000.0;

        for (const auto &track : catalogs.engine) {
            if (attempted >= sampleSize) {
                break;
            }
            ++attempted;
            try {
                writer.writeHotCues(track.sourceId, {cue});
            } catch (const std::exception &) {
                ++refused;
            }
        }
    }
    // A refusal must not be silent, and it must not take the rest of the
    // batch with it: everything after a refused track still has to land.
    check(attempted == std::min<int>(sampleSize, static_cast<int>(catalogs.engine.size())),
          "the whole batch was attempted despite refusals");
    std::cout << "    " << refused << " of " << attempted << " write(s) refused\n";
    // Recorded rather than asserted to be zero: 30 of 50 tracks refused
    // the rejected snapshot-based write path on a real stick, so a
    // library where this is nonzero is a property to track, not a bug.
    expected.expect("engine.writeRefusals", refused, "Engine write refusals did not increase");
    fs::remove_all(root);
    pass("stability: Engine writes degrade visibly, one track at a time");
}

// ------------------------------------------------------- work-count case

// The portable half of performance testing. These are the numbers the
// optimisation work in docs/write-path-performance.md exists to reduce:
// today every item reopens its database, and this records that so the
// improvement is visible and cannot silently regress afterwards.
void caseWorkCounts(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs, int items,
                    Expectations &expected)
{
    if (catalogs.engine.empty()) {
        return;
    }
    const fs::path root = scratch / "engine-counts";
    fs::remove_all(root);
    fs::copy(*set.engineRoot, root, fs::copy_options::recursive);

    if (static_cast<int>(catalogs.engine.size()) < items) {
        items = static_cast<int>(catalogs.engine.size());
    }

    WorkCounters::instance().reset();
    // Scoped so the writer's own SQLite connection is closed before
    // remove_all() below deletes the tree -- see caseEngineStability's
    // own comment on the same fix.
    {
        infrastructure::engine::LibdjinteropEngineCueWriter writer(root.string());
        domain::CuePoint cue;
        cue.kind = domain::CuePoint::Kind::Hot;
        cue.hotCueNumber = 1;
        cue.positionMs = 2000.0;

        for (int i = 0; i < items; ++i) {
            try {
                writer.writeHotCues(catalogs.engine[static_cast<size_t>(i)].sourceId, {cue});
            } catch (const std::exception &) {
                // counted by the stability case, not here
            }
        }
    }
    const auto counts = WorkCounters::instance().snapshot();
    std::cout << "    " << items << " Engine cue writes: " << counts.describe() << "\n";

    // The total for the batch, not a per-item average. The writer now
    // holds its handle for the save, so this is 1 however many items the
    // batch has, and an average would round that win down to zero.
    expected.expect("engine.opensPerBatch", counts.engineDatabaseOpens,
                    "Engine database opens for the whole batch unchanged");
    fs::remove_all(root);
    pass("work counts: the per-item database opens match what is documented");
}

}  // namespace

// ---------------------------------------------------------- matrix cases
//
// Cue positions round-trip through each format's own units -- frames in
// one, integer milliseconds in another -- so comparing them needs a
// tolerance. Exact double equality looks stricter and is simply wrong: it
// fails on a cue that landed perfectly.
constexpr double CuePositionToleranceMs = 1.0;

inline bool samePosition(double a, double b)
{
    return std::abs(a - b) <= CuePositionToleranceMs;
}

// One case per editing feature, each built the same way: copy the set into
// scratch, stage the change class the page stages, run it through the real
// save loop, then construct a FRESH reader and assert on what comes back.
// A test that trusts the writer's own return value is not a test -- every
// write-path bug this project has found was invisible to one.
//
// These need the change classes, so they need Qt Core (see
// SEABASS_CORPUS_HAS_EDIT in CMakeLists.txt). Everything above does not.

#ifdef SEABASS_CORPUS_HAS_EDIT

namespace
{

// A save, driven exactly as a page drives one. The overload taking a token
// lets a case cancel itself partway through, which is the only way to
// observe what a half-finished save left behind.
gui::SaveLoopResult runChanges(const std::vector<std::shared_ptr<gui::PendingChange>> &changes,
                               const fs::path &rekordboxRoot, const fs::path &engineRoot,
                               application::CancellationToken cancel)
{
    gui::SaveContext ctx(cancel, application::NullProgressReporter::instance(), nullptr,
                         QString::fromStdString(rekordboxRoot.string()),
                         QString::fromStdString(engineRoot.string()));
    return runSaveLoop(changes, ctx);
}

gui::SaveLoopResult runChanges(const std::vector<std::shared_ptr<gui::PendingChange>> &changes,
                               const fs::path &rekordboxRoot, const fs::path &engineRoot)
{
    application::CancellationToken cancel;
    gui::SaveContext ctx(cancel, application::NullProgressReporter::instance(), nullptr,
                         QString::fromStdString(rekordboxRoot.string()),
                         QString::fromStdString(engineRoot.string()));
    return runSaveLoop(changes, ctx);
}

std::vector<domain::Track> rescanRekordbox(const fs::path &root)
{
    infrastructure::rekordbox::KaitaiRekordboxReader reader(root.string());
    return application::ScanLibrary(reader).execute();
}

std::vector<domain::Track> rescanEngine(const fs::path &root)
{
    infrastructure::engine::LibdjinteropEngineReader reader(root.string());
    return application::ScanLibrary(reader).execute();
}

const domain::Track *findTrack(const std::vector<domain::Track> &tracks, const std::string &sourceId)
{
    for (const auto &t : tracks) {
        if (t.sourceId == sourceId) {
            return &t;
        }
    }
    return nullptr;
}

int countJunkCues(const std::vector<domain::Track> &tracks)
{
    int total = 0;
    for (const auto &t : tracks) {
        for (const auto &c : t.cues) {
            if (c.kind == domain::CuePoint::Kind::Memory && c.positionMs == 0.0) {
                ++total;
            }
        }
    }
    return total;
}

}  // namespace

// Matrix: Add cue. The cue reads back at the same position and colour, and
// writing a second cue to the same hot slot replaces it rather than
// leaving the pad claiming two.
void caseAddCue(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs, Expectations &expected)
{
    if (catalogs.rekordbox.empty()) {
        return;
    }
    const domain::Track *target = nullptr;
    for (const auto &t : catalogs.rekordbox) {
        if (t.cues.empty()) {
            target = &t;
            break;
        }
    }
    if (target == nullptr) {
        std::cout << "    skipped matrix/add-cue: no track without cues\n";
        return;
    }
    const std::string id = target->sourceId;
    const fs::path root = freshRekordboxCopy(set, scratch, "matrix-addcue");

    WorkCounters::instance().reset();
    auto change = std::make_shared<gui::AddCueChange>("rekordbox", QString::fromStdString(root.string()),
                                                      QString::fromStdString(id), 45000.0, "hot", 2, "#00FF00", "",
                                                      false, 0.0, QString::fromStdString(target->title));
    auto result = runChanges({change}, root, {});
    const auto counts = WorkCounters::instance().snapshot();

    if (!check(result.error.isEmpty(), "the add-cue save reported no error: " + result.error.toStdString())) {
        fs::remove_all(root);
        return;
    }
    auto after = rescanRekordbox(root);
    const domain::Track *reread = findTrack(after, id);
    if (check(reread != nullptr, "the track survived the add-cue save")) {
        if (check(reread->cues.size() == 1, "exactly one cue came back")) {
            check(samePosition(reread->cues[0].positionMs, 45000.0),
                  "the cue is at the position it was written at");
            check(reread->cues[0].hotCueNumber == 2, "the cue is in the hot slot it was written to");

            // Add Cue mirrors onto the OneLibrary copy of the same
            // library. Nothing asserted that until now, and it was
            // silently doing nothing whenever the track's path came back
            // space-padded from export.pdb -- see toContentPath().
            if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(root.string())) {
                auto trimmed = [](std::string path) {
                    while (!path.empty() && path.back() == ' ') {
                        path.pop_back();
                    }
                    return fs::path(path).filename().string();
                };
                try {
                    infrastructure::onelibrary::OneLibraryReader reader(root.string());
                    // Named, not a temporary bound into the range-for: a
                    // pointer taken into readAll()'s returned vector must
                    // outlive the loop that fills `mirrored`, and a
                    // temporary's lifetime ends with the loop itself,
                    // leaving mirrored dangling the moment it is read.
                    // Reads through it come back as plausible-looking
                    // garbage -- a hot-cue number that differs between runs
                    // of identical input, which cost a night's debugging.
                    const std::vector<domain::Track> oneLibraryTracks = reader.readAll();
                    const std::string wanted = trimmed(reread->filePath);
                    const domain::Track *mirrored = nullptr;
                    for (const auto &t : oneLibraryTracks) {
                        if (trimmed(t.filePath) == wanted) {
                            mirrored = &t;
                            break;
                        }
                    }
                    if (mirrored != nullptr) {
                        bool landed = false;
                        for (const auto &c : mirrored->cues) {
                            if (c.kind == domain::CuePoint::Kind::Hot && c.hotCueNumber == 2
                                && samePosition(c.positionMs, 45000.0)) {
                                landed = true;
                            }
                        }
                        check(landed, "the OneLibrary copy of the track has the added cue in the same hot slot");
                    } else {
                        std::cout << "    add cue: no OneLibrary row for this track; mirror not exercised\n";
                    }
                } catch (const std::exception &e) {
                    check(false, std::string("could not read OneLibrary after Add Cue: ") + e.what());
                }
            }
        }
    }

    // Same slot again, a different position: a hardware pad holds one cue,
    // so this replaces rather than adds.
    auto replacement = std::make_shared<gui::AddCueChange>("rekordbox", QString::fromStdString(root.string()),
                                                           QString::fromStdString(id), 60000.0, "hot", 2, "#0000FF", "",
                                                           false, 0.0, QString::fromStdString(target->title));
    auto second = runChanges({replacement}, root, {});
    check(second.error.isEmpty(), "the replacing save reported no error");
    auto afterReplace = rescanRekordbox(root);
    const domain::Track *replaced = findTrack(afterReplace, id);
    if (check(replaced != nullptr, "the track survived the replacing save")) {
        if (check(replaced->cues.size() == 1, "the hot slot still holds exactly one cue")) {
            check(replaced->cues[0].positionMs == 60000.0, "the slot holds the newer cue");
        }
    }

    expected.expect("matrix.addCue.pdbParses", counts.trackDatabaseParses, "add-cue pdb parses per item unchanged");
    expected.expect("matrix.addCue.durableWritesPerSave", counts.durableFileWrites,
                    "add-cue durable whole-file writes for the whole save unchanged");
    std::cout << "    add cue: " << counts.describe() << "\n";
    fs::remove_all(root);
    pass("matrix: add cue reads back, and a hot slot holds one cue");
}

// Matrix: stray cue removal. After the save a fresh scan finds none on the
// touched tracks, and the tracks' other cues are untouched.
// Every file a save actually changed must be in its backup record.
//
// filesToBackup() is a declaration, and a declaration can be wrong in two
// directions. Too many files is loud -- Undo restores something the save
// never touched, and the resolver test covers it. Too few is silent: the
// save overwrites a file nothing backed up, every write succeeds, and the
// damage only shows when someone tries to undo.
//
// So rather than trusting any change class to enumerate itself, this
// hashes the whole stick before and after a real save and asserts that
// whatever moved is in the record. It covers every workflow the matrix
// runs, including ones nobody thought to check.
void caseBackupCoversEveryChangedFile(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs)
{
    std::vector<const domain::Track *> withJunk;
    for (const auto &t : catalogs.rekordbox) {
        for (const auto &c : t.cues) {
            if (c.kind == domain::CuePoint::Kind::Memory && c.positionMs == 0.0) {
                withJunk.push_back(&t);
                break;
            }
        }
        if (withJunk.size() >= 3) {
            break;
        }
    }
    if (withJunk.empty()) {
        std::cout << "    skipped matrix/backup-covers-changes: no 0:00 memory cues in this library\n";
        return;
    }

    const fs::path stickRoot = scratch / "matrix-covers-stick";
    fs::remove_all(stickRoot);
    fs::create_directories(stickRoot);
    const fs::path root = stickRoot / fs::path(*set.rekordboxRoot).filename();
    fs::copy(*set.rekordboxRoot, root, fs::copy_options::recursive);

    // Content before, for every file the save could possibly touch.
    auto snapshot = [](const fs::path &under) {
        std::map<std::string, std::string> byPath;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(under, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                byPath[it->path().string()] = readWholeFile(it->path());
            }
        }
        return byPath;
    };
    const auto before = snapshot(root);

    std::vector<std::shared_ptr<gui::PendingChange>> changes;
    for (const auto *t : withJunk) {
        domain::Track copy = *t;
        changes.push_back(std::make_shared<gui::RemoveJunkCueChange>(QString::fromStdString(root.string()), copy));
    }
    auto result = runChanges(changes, root, {});
    if (!check(result.error.isEmpty(), "the save reported no error: " + result.error.toStdString())) {
        fs::remove_all(stickRoot);
        return;
    }

    const auto after = snapshot(root);

    std::set<std::string> backedUp;
    infrastructure::backup::FilesystemBackupStore store(
        infrastructure::backup::backupDirForCatalogPath(root.string()));
    for (const auto &record : store.list()) {
        for (const auto &recorded : record.filePaths) {
            backedUp.insert(fs::weakly_canonical(fs::path(recorded)).string());
        }
    }

    std::vector<std::string> unbacked;
    for (const auto &[path, contentBefore] : before) {
        auto now = after.find(path);
        if (now == after.end() || now->second == contentBefore) {
            continue;  // untouched, or removed (a removal is a different property)
        }
        if (!backedUp.count(fs::weakly_canonical(fs::path(path)).string())) {
            unbacked.push_back(path);
        }
    }

    if (!check(unbacked.empty(),
               "every file the save changed is in its backup record ("
                   + std::to_string(unbacked.size()) + " changed with no backup)")) {
        for (const auto &path : unbacked) {
            std::cout << "      overwritten with nothing to restore it from: " << path << "\n";
        }
    }

    fs::remove_all(stickRoot);
    pass("matrix: a save backs up every file it changes");
}

// The path-only resolver every workflow's filesToBackup() is built on.
// It decides which file a write will overwrite, so a wrong answer means the
// save backs up one file and overwrites another -- silently, because every
// write still succeeds.
//
// Checked against the real fixture rather than a synthetic one: the
// rekordbox branch resolves a track id through export.pdb, which is
// exactly the step that can go wrong.
// export.pdb keeps strings in fixed-length fields and right-pads them.
// Every string the reader hands out must already be trimmed, because
// three destructive decisions compare these against paths walked off the
// filesystem, which are not padded: a padded catalog path makes every
// referenced file look unreferenced, and unreferenced files are offered
// for deletion.
//
// Against the real corpus rather than a synthetic string, because the
// padding is a property of what rekordbox actually wrote.
void caseNoPaddedStringsFromRekordbox(const DataSet &set, const Catalogs &catalogs)
{
    (void)set;
    auto padded = [](const std::string &value) {
        return !value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\0');
    };
    std::size_t paddedPaths = 0, paddedText = 0;
    for (const auto &track : catalogs.rekordbox) {
        if (padded(track.filePath) || padded(track.filename)) {
            ++paddedPaths;
        }
        if (padded(track.title) || padded(track.artist)) {
            ++paddedText;
        }
    }
    check(paddedPaths == 0, "rekordbox track paths must not carry the format's field padding");
    check(paddedText == 0, "rekordbox track titles/artists must not carry the format's field padding");

    // And the property that actually matters: a catalog path and the same
    // path as it would come off the filesystem key the same.
    std::size_t agreed = 0;
    for (const auto &track : catalogs.rekordbox) {
        if (track.filePath.empty()) {
            continue;
        }
        if (application::normalizedPathKey(track.filePath)
            == application::normalizedPathKey(fs::path(track.filePath).lexically_normal().string())) {
            ++agreed;
        }
    }
    check(agreed == catalogs.rekordbox.size(),
          "every rekordbox path must key the same as its on-disk spelling");
}

// Collapse turns rows into files: one audio file listed by both rekordbox
// and Engine must come back as ONE entry carrying two catalogRows, one per
// format. That is the whole premise of treating a file rather than a row
// as the unit of duplication, and everything the cleanup writer does with
// catalogRows rests on it -- including the refusal that stops a save
// removing a file's row from one catalog while leaving the others pointing
// at it.
//
// Asserted against a real catalog rather than a synthetic pair on purpose.
// A hand-written pair would have both paths spelled identically and would
// have passed throughout the period when this was actually broken: until
// rekordbox strings stopped arriving space-padded, a rekordbox path keyed
// differently from the Engine path naming the same file, so collapse
// produced one entry per row and never grouped across formats at all.
// Nothing was green-while-checking-nothing in the usual sense -- the code
// was simply never handed a real catalog path. Only a real one catches it.
// Collapse turns rows into files: one audio file listed by both rekordbox
// and Engine must come back as ONE entry carrying two catalogRows, one per
// format. That is the premise of treating a file rather than a row as the
// unit of duplication, and everything the cleanup writer does with
// catalogRows rests on it -- including the refusal that stops a save
// removing a file's row from one catalog while leaving the others pointing
// at it.
//
// Against a real catalog rather than a synthetic pair on purpose: a
// hand-written pair has both paths spelled identically by construction and
// would pass even while real grouping was broken, which is how the
// space-padded rekordbox paths went unnoticed for so long.
//
// The committed fixture cannot carry this property, and says so rather
// than quietly passing. Its extraction stored Engine's copy of every file
// under engine/Contents/ while rekordbox records Contents/, so the two
// catalogs describe the same 1161 files under paths that share no prefix.
// On a real stick both catalogs name the same file under the same root,
// which is the case that matters and the one no committed fixture has.
void caseCollapseGroupsAcrossFormats(const DataSet &set, const Catalogs &catalogs)
{
    (void)set;
    if (catalogs.rekordbox.empty() || catalogs.engine.empty()) {
        // Not a skipped check, an absent property: a set with one catalog
        // has no cross-format grouping to get right.
        std::cout << "    (no cross-format grouping to check: this set has only one catalog)\n";
        return;
    }

    std::set<std::string> rekordboxKeys, engineKeys, rekordboxNames, engineNames;
    for (const auto &track : catalogs.rekordbox) {
        rekordboxKeys.insert(application::normalizedPathKey(track.filePath));
        rekordboxNames.insert(fs::path(track.filePath).filename().string());
    }
    for (const auto &track : catalogs.engine) {
        engineKeys.insert(application::normalizedPathKey(track.filePath));
        engineNames.insert(fs::path(track.filePath).filename().string());
    }
    std::size_t sharedPaths = 0, sharedNames = 0;
    for (const auto &key : rekordboxKeys) {
        if (engineKeys.count(key)) {
            ++sharedPaths;
        }
    }
    for (const auto &name : rekordboxNames) {
        if (engineNames.count(name)) {
            ++sharedNames;
        }
    }

    if (sharedPaths == 0) {
        // Stated, not skipped silently. The weaker fact is still worth
        // asserting: if the two catalogs stopped describing the same
        // files at all, every cross-catalog case in this suite would be
        // comparing unrelated libraries and would need to be re-read.
        std::cout << "    (cross-format grouping not exercisable here: the two catalogs share "
                  << sharedNames << " filenames but no full path)\n";
        check(sharedNames > 0,
              "the two catalogs of one library must at least describe the same files by name");
        return;
    }

    std::vector<domain::Track> rows;
    rows.reserve(catalogs.rekordbox.size() + catalogs.engine.size());
    rows.insert(rows.end(), catalogs.rekordbox.begin(), catalogs.rekordbox.end());
    rows.insert(rows.end(), catalogs.engine.begin(), catalogs.engine.end());

    const auto files = application::collapseCatalogRows(rows);

    // Counting entries whose rows name more than one DISTINCT FORMAT, not
    // merely more than one row: two rekordbox rows for one file collapse
    // together too, and that is a different property. A stick really can
    // hold duplicate rows within one catalog -- that is what the cleanup
    // page exists for -- so a size check alone would pass while the
    // cross-format case stayed broken.
    std::size_t crossFormat = 0;
    for (const auto &file : files) {
        std::set<std::string> formats;
        for (const auto &row : file.catalogRows) {
            formats.insert(row.format);
        }
        if (formats.size() > 1) {
            ++crossFormat;
        }
    }

    check(crossFormat > 0,
          "collapsing a rekordbox catalog and an Engine catalog that share file paths must produce "
          "at least one file carrying rows from both formats");
    check(files.size() < rows.size(),
          "collapsing two catalogs that share file paths must yield fewer files than rows");
    std::cout << "    collapse: " << rows.size() << " rows -> " << files.size() << " files, "
              << crossFormat << " carrying rows from more than one format\n";
}

void caseBackupPathResolver(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs)
{
    if (catalogs.rekordbox.empty()) {
        std::cout << "    skipped matrix/path-resolver: no rekordbox catalog in this set\n";
        return;
    }
    const fs::path root = freshRekordboxCopy(set, scratch, "matrix-resolver");

    application::CancellationToken cancel;
    gui::SaveContext ctx(cancel, application::NullProgressReporter::instance(), nullptr,
                         QString::fromStdString(root.string()), QString());
    const QString qroot = QString::fromStdString(root.string());

    const domain::Track &track = catalogs.rekordbox.front();
    const domain::TrackId id{"rekordbox", track.sourceId};

    auto has = [](const std::vector<std::string> &files, const std::string &needle) {
        for (const auto &f : files) {
            if (f.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    const auto cuesOnly = gui::filesWrittenFor(gui::WriteScope{}, id, qroot, ctx);
    check(has(cuesOnly, ".EXT"), "a cue write names this track's analysis file");
    check(!has(cuesOnly, "export.pdb"),
          "a cue write does NOT name export.pdb -- rekordbox keeps cues outside the catalog, and backing it "
          "up would put a file Undo restores but the save never changed into the record");

    const auto withRows = gui::filesWrittenFor(gui::WriteScope{.catalogRows = true}, id, qroot, ctx);
    check(has(withRows, ".EXT"), "a row-rewriting write still names the analysis file");
    check(has(withRows, "export.pdb"), "a row-rewriting write also names export.pdb");

    // The resolver must agree with the lookup the writers themselves use.
    const auto direct = infrastructure::rekordbox::findAnlzPathForTrackId(
        root.string(), static_cast<std::uint32_t>(std::stoul(track.sourceId)));
    if (check(direct.has_value(), "the fixture resolves this track's analysis path directly")) {
        const std::string expected = infrastructure::rekordbox::extAnlzPath(root.string(), *direct);
        check(has(cuesOnly, fs::path(expected).parent_path().filename().string()),
              "the resolver names the same analysis file the writers would open");
    }

    // A sourceId that is not a number at all: report nothing rather than
    // guess a path from it.
    check(gui::filesWrittenFor(gui::WriteScope{}, {"rekordbox", "not-a-track-id"}, qroot, ctx).empty(),
          "an unusable sourceId resolves to no files rather than to a wrong one");

    // A well-formed id no catalog holds: no analysis file, and in
    // particular not some neighbouring track's.
    const auto missing = gui::filesWrittenFor(gui::WriteScope{}, {"rekordbox", "4294967000"}, qroot, ctx);
    check(!has(missing, ".EXT"), "an id absent from the catalog resolves to no analysis file");

    // The OneLibrary mirror is opt-in per workflow, not inferred from the
    // database being present. Sync leaves it alone even on a stick that
    // has one; Add Cue writes it. Both directions are defects.
    const auto mirrored = gui::filesWrittenFor(gui::WriteScope{.oneLibraryMirror = true}, id, qroot, ctx);
    if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(root.string())) {
        check(has(mirrored, "exportLibrary.db"), "a mirroring write names the OneLibrary database");
        check(!has(cuesOnly, "exportLibrary.db"),
              "a non-mirroring write does NOT name it, even though the stick has one");
    }

    // Engine names one shared database whatever the track.
    const auto engine = gui::filesWrittenFor(gui::WriteScope{}, {"engine", track.sourceId},
                                             QString::fromStdString((root / "Engine Library").string()), ctx);
    check(has(engine, "m.db"), "an Engine write names m.db");

    fs::remove_all(root);
    pass("matrix: the backup path resolver names what a write would touch, and nothing else");
}

// Wraps a change so the save cancels itself once `cancelAfter` of them have
// been applied. Everything else is delegated, filesToBackup() included, so
// the wrapped change behaves exactly as the page stages it.
class CancelAfterApplies : public gui::PendingChange
{
public:
    CancelAfterApplies(std::shared_ptr<gui::PendingChange> inner, application::CancellationToken cancel,
                       std::shared_ptr<int> applied, int cancelAfter)
        : m_inner(std::move(inner)), m_cancel(std::move(cancel)), m_applied(std::move(applied)),
          m_cancelAfter(cancelAfter)
    {
    }

    QString id() const override { return m_inner->id(); }
    QString description() const override { return m_inner->description(); }
    QString unit() const override { return m_inner->unit(); }
    QStringList formatsTouched() const override { return m_inner->formatsTouched(); }
    QString owner() const override { return m_inner->owner(); }
    std::vector<gui::BackupTarget> filesToBackup(gui::SaveContext &ctx) const override
    {
        return m_inner->filesToBackup(ctx);
    }
    gui::ChangeOutcome apply(gui::SaveContext &ctx) override
    {
        gui::ChangeOutcome outcome = m_inner->apply(ctx);
        if (++(*m_applied) >= m_cancelAfter) {
            m_cancel.cancel();
        }
        return outcome;
    }

private:
    std::shared_ptr<gui::PendingChange> m_inner;
    application::CancellationToken m_cancel;
    std::shared_ptr<int> m_applied;
    int m_cancelAfter;
};

// The property the whole backup-first conversion exists for, and the one no
// work counter can see: when a save is interrupted partway, the backup must
// already describe EVERY file the save was going to touch -- not only the
// ones it got to.
//
// Without it, a stick pulled after item n leaves items 1..n rewritten and
// backed up, items n+1.. untouched and absent, and no single record that
// returns the library to where it started. The count of durable writes is
// identical either way, which is exactly why this needs its own guard: for
// a single-file change the conversion cannot be seen in the counters at all.
void caseBackupPrecedesWrites(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs)
{
    std::vector<const domain::Track *> withJunk;
    for (const auto &t : catalogs.rekordbox) {
        for (const auto &c : t.cues) {
            if (c.kind == domain::CuePoint::Kind::Memory && c.positionMs == 0.0) {
                withJunk.push_back(&t);
                break;
            }
        }
        if (withJunk.size() >= 4) {
            break;
        }
    }
    if (withJunk.size() < 3) {
        std::cout << "    skipped matrix/backup-ordering: needs at least 3 tracks with a 0:00 memory cue\n";
        return;
    }
    // Its own stick root, not the shared scratch one: every other matrix
    // case copies into scratch/<name>, so they all share one
    // .seabass-backups and this case would happily count another case's
    // record as its own. That is how the first version of this test passed
    // while the property it checks was disabled.
    const fs::path stickRoot = scratch / "matrix-ordering-stick";
    fs::remove_all(stickRoot);
    fs::create_directories(stickRoot);
    const fs::path root = stickRoot / fs::path(*set.rekordboxRoot).filename();
    fs::copy(*set.rekordboxRoot, root, fs::copy_options::recursive);

    application::CancellationToken cancel;
    auto applied = std::make_shared<int>(0);
    const int cancelAfter = 2;

    std::vector<std::shared_ptr<gui::PendingChange>> changes;
    std::set<std::string> declared;
    for (const auto *t : withJunk) {
        domain::Track copy = *t;
        auto inner = std::make_shared<gui::RemoveJunkCueChange>(QString::fromStdString(root.string()), copy);
        changes.push_back(std::make_shared<CancelAfterApplies>(inner, cancel, applied, cancelAfter));
    }

    auto result = runChanges(changes, root, {}, cancel);
    check(result.cancelled, "the save reported itself cancelled");
    check(static_cast<int>(result.appliedIds.size()) == cancelAfter,
          "exactly " + std::to_string(cancelAfter) + " of " + std::to_string(changes.size())
              + " changes were applied before the interruption");

    // What did the backup record end up describing?
    // `root` is the PIONEER folder; the backups sit beside it under the
    // stick root, which is its parent.
    infrastructure::backup::FilesystemBackupStore store(
        infrastructure::backup::backupDirForCatalogPath(root.string()));
    std::set<std::string> backedUp;
    for (const auto &record : store.list()) {
        if (record.label != "junk-cue-cleanup") {
            continue;
        }
        for (const auto &recorded : record.filePaths) {
            const fs::path p(recorded);
            // Analysis files only. The save also backs up the OneLibrary
            // mirror, which is one file however many tracks are touched --
            // counting it would make the threshold mean something other
            // than "one per track".
            if (p.extension() != ".EXT") {
                continue;
            }
            // Every one of them is named ANLZ0000.EXT; the containing
            // directory is what tells them apart.
            backedUp.insert(p.parent_path().filename().string());
        }
    }

    // One entry per track, whichever the save reached. Analysis files are
    // all named ANLZ0000.EXT, so they are counted by their containing
    // directory, which is what distinguishes them.
    const size_t expectedFiles = withJunk.size();
    if (!check(backedUp.size() == expectedFiles,
               "the backup describes all " + std::to_string(expectedFiles) + " analysis files the save would touch, not the "
                   + std::to_string(cancelAfter) + " it reached (found " + std::to_string(backedUp.size()) + ")")) {
        std::cout << "      the interrupted save left a backup covering only part of what it set out to change\n";
    }

    fs::remove_all(stickRoot);
    pass("matrix: an interrupted save has already backed up everything it meant to touch");
}

void caseStrayCueRemoval(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs, Expectations &expected)
{
    std::vector<const domain::Track *> withJunk;
    for (const auto &t : catalogs.rekordbox) {
        for (const auto &c : t.cues) {
            if (c.kind == domain::CuePoint::Kind::Memory && c.positionMs == 0.0) {
                withJunk.push_back(&t);
                break;
            }
        }
        if (withJunk.size() >= 5) {
            break;
        }
    }
    if (withJunk.empty()) {
        std::cout << "    skipped matrix/stray-cue: this library has no 0:00 memory cues\n";
        return;
    }
    const fs::path root = freshRekordboxCopy(set, scratch, "matrix-stray");

    std::vector<std::shared_ptr<gui::PendingChange>> changes;
    std::map<std::string, size_t> cuesBefore;
    for (const auto *t : withJunk) {
        domain::Track copy = *t;
        cuesBefore[t->sourceId] = t->cues.size();
        changes.push_back(std::make_shared<gui::RemoveJunkCueChange>(QString::fromStdString(root.string()), copy));
    }

    WorkCounters::instance().reset();
    auto result = runChanges(changes, root, {});
    const auto counts = WorkCounters::instance().snapshot();
    if (!check(result.error.isEmpty(), "the stray-cue save reported no error: " + result.error.toStdString())) {
        fs::remove_all(root);
        return;
    }
    check(result.appliedIds.size() == changes.size(), "every staged removal was applied");

    auto after = rescanRekordbox(root);
    for (const auto &[id, before] : cuesBefore) {
        const domain::Track *reread = findTrack(after, id);
        if (!check(reread != nullptr, "track " + id + " survived the stray-cue save")) {
            continue;
        }
        int junk = 0;
        for (const auto &c : reread->cues) {
            if (c.kind == domain::CuePoint::Kind::Memory && c.positionMs == 0.0) {
                ++junk;
            }
        }
        check(junk == 0, "track " + id + " has no 0:00 memory cue left");
        // Exactly the stray cue went, and nothing else with it.
        check(reread->cues.size() == before - 1, "track " + id + " kept every other cue it had");
    }
    // The total for the whole save, not a per-item average: the point of
    // the index is that this stays flat as the batch grows, and a
    // per-item figure rounds that win down to zero.
    expected.expect("matrix.strayCue.pdbParsesPerSave", counts.trackDatabaseParses,
                    "stray-cue pdb parses for the whole save unchanged");
    // Also flat in the batch size, and the one that actually costs
    // wall-clock: every SQLCipher open derives the key from a passphrase,
    // about 115 ms of CPU that no faster disk helps with.
    expected.expect("matrix.strayCue.encryptedOpensPerSave", counts.encryptedDatabaseOpens,
                    "stray-cue SQLCipher opens for the whole save unchanged");
    // The number this whole write-path effort is about, and until now the
    // only one of the four that was printed but never asserted -- so the
    // change from a durable write per backed-up file to one deflated
    // archive could have landed without anything noticing either way.
    // Batch total, not a per-item average: an average rounds a 200-to-1
    // win down to nothing.
    expected.expect("matrix.strayCue.durableWritesPerSave", counts.durableFileWrites,
                    "stray-cue durable whole-file writes for the whole save unchanged");
    std::cout << "    stray cue removal (" << changes.size() << " tracks): " << counts.describe() << "\n";
    fs::remove_all(root);
    pass("matrix: stray cues go, and only they go");
}

// Matrix: Sync. Every planned cue lands on the target and reads back
// equal. Runs against both catalogs, so the source is real data and the
// target is a real catalog of a different format.
void caseSync(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs, Expectations &expected)
{
    if (catalogs.rekordbox.empty() || catalogs.engine.empty()) {
        std::cout << "    skipped matrix/sync: this set has only one catalog\n";
        return;
    }
    const fs::path rekordboxRoot = freshRekordboxCopy(set, scratch, "matrix-sync-rb");
    const fs::path engineRoot = scratch / "matrix-sync-engine";
    fs::remove_all(engineRoot);
    fs::copy(*set.engineRoot, engineRoot, fs::copy_options::recursive);

    // Plan against the COPIES, not the originals. These changes are applied
    // to these roots, and a track's filePath is what OneLibrary resolves a
    // row by -- planning from the original scan hands every change a path
    // outside the stick it writes to, so the OneLibrary half of a rekordbox
    // write finds no row and silently does nothing. On a real stick the
    // scan and the save share a root; this makes the harness match that.
    std::vector<domain::Track> rekordboxHere = rescanRekordbox(rekordboxRoot);
    std::vector<domain::Track> engineHere;
    try {
        engineHere = rescanEngine(engineRoot);
    } catch (const std::exception &e) {
        check(false, std::string("could not read the copied Engine catalog: ") + e.what());
        fs::remove_all(rekordboxRoot);
        fs::remove_all(engineRoot);
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto plans = application::SyncLibraries().execute(rekordboxHere, engineHere, now, now);

    // Plans carrying at least one HOT cue, preferred over ones carrying
    // only memory cues at 0:00. Formats disagree about memory cues by
    // design -- Engine keeps one whatever you write -- so a plan made only
    // of those can verify nothing on either the target or its mirror, and
    // the fixture has plenty of them.
    std::vector<domain::SyncPlan> withCues;
    std::vector<domain::SyncPlan> memoryOnly;
    for (const auto &plan : plans) {
        if (plan.cuesToApply.empty()) {
            continue;
        }
        bool hasHot = false;
        for (const auto &c : plan.cuesToApply) {
            if (c.kind == domain::CuePoint::Kind::Hot) {
                hasHot = true;
            }
        }
        if (hasHot && withCues.size() < 5) {
            withCues.push_back(plan);
        } else if (!hasHot && memoryOnly.size() < 5) {
            memoryOnly.push_back(plan);
        }
        if (withCues.size() >= 5) {
            break;
        }
    }
    if (withCues.empty()) {
        std::cout << "    matrix/sync: no plan in this set carries a hot cue; falling back to memory-only plans, "
                     "which cannot verify a cue landed\n";
        withCues = memoryOnly;
    }
    if (withCues.empty()) {
        std::cout << "    skipped matrix/sync: no plan carries cues to write\n";
        fs::remove_all(rekordboxRoot);
        fs::remove_all(engineRoot);
        return;
    }

    std::vector<std::shared_ptr<gui::PendingChange>> changes;
    for (const auto &plan : withCues) {
        changes.push_back(std::make_shared<gui::SyncPlanChange>(QString::fromStdString(rekordboxRoot.string()),
                                                                QString::fromStdString(engineRoot.string()), plan,
                                                                static_cast<int>(withCues.size())));
    }

    WorkCounters::instance().reset();
    auto result = runChanges(changes, rekordboxRoot, engineRoot);
    const auto counts = WorkCounters::instance().snapshot();
    if (!check(result.error.isEmpty(), "the sync save reported no error: " + result.error.toStdString())) {
        fs::remove_all(rekordboxRoot);
        fs::remove_all(engineRoot);
        return;
    }

    auto rekordboxAfter = rescanRekordbox(rekordboxRoot);
    std::vector<domain::Track> engineAfter;
    try {
        engineAfter = rescanEngine(engineRoot);
    } catch (const std::exception &e) {
        check(false, std::string("could not re-read the Engine catalog after the sync: ") + e.what());
    }

    for (const auto &plan : withCues) {
        const domain::Track &target =
            plan.direction == domain::SyncPlan::Direction::ToB ? plan.match.trackB : plan.match.trackA;
        const auto &after = target.format == "engine" ? engineAfter : rekordboxAfter;
        const domain::Track *reread = findTrack(after, target.sourceId);
        if (!check(reread != nullptr, "sync target " + target.sourceId + " survived the save")) {
            continue;
        }
        // Every hot cue the plan carried must be there at its position.
        // Engine keeps one memory cue by design, so only hot cues are
        // asserted one for one.
        for (const auto &planned : plan.cuesToApply) {
            if (planned.kind != domain::CuePoint::Kind::Hot) {
                continue;
            }
            bool landed = false;
            for (const auto &actual : reread->cues) {
                if (actual.kind == domain::CuePoint::Kind::Hot && actual.hotCueNumber == planned.hotCueNumber
                    && samePosition(actual.positionMs, planned.positionMs)) {
                    landed = true;
                }
            }
            check(landed, "planned hot cue " + std::to_string(planned.hotCueNumber) + " landed on "
                              + target.format + " track " + target.sourceId);
        }
    }
    // Three formats, one library. export.pdb and exportLibrary.db are the
    // same rekordbox library written twice, so a cue synced onto one and
    // not the other leaves rekordbox 7 disagreeing with the older export
    // about the same track. Every other cue-writing workflow mirrors;
    // Sync did not until this was asserted.
    if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rekordboxRoot.string())) {
        std::vector<domain::Track> oneLibraryAfter;
        try {
            infrastructure::onelibrary::OneLibraryReader reader(rekordboxRoot.string());
            oneLibraryAfter = reader.readAll();
        } catch (const std::exception &e) {
            check(false, std::string("could not re-read OneLibrary after the sync: ") + e.what());
        }
        int checked = 0;
        for (const auto &plan : withCues) {
            const domain::Track &target =
                plan.direction == domain::SyncPlan::Direction::ToB ? plan.match.trackB : plan.match.trackA;
            if (target.format != "rekordbox" || target.filePath.empty()) {
                continue;
            }
            // Matched on basename, not full path. The two catalogs record
            // the same file differently -- and in this fixture the
            // rekordbox path is space-padded, because the anonymizer must
            // preserve each field's original byte length.
            auto basename = [](std::string path) {
                while (!path.empty() && path.back() == ' ') {
                    path.pop_back();
                }
                return fs::path(path).filename().string();
            };
            const std::string wanted = basename(target.filePath);
            const domain::Track *mirrored = nullptr;
            for (const auto &t : oneLibraryAfter) {
                if (basename(t.filePath) == wanted) {
                    mirrored = &t;
                }
            }
            if (mirrored == nullptr) {
                continue;  // this track has no OneLibrary row at all
            }
            // Hot cues only, for the same reason the target check above
            // uses them: the formats disagree about memory cues by design.
            for (const auto &planned : plan.cuesToApply) {
                if (planned.kind != domain::CuePoint::Kind::Hot) {
                    continue;
                }
                bool landed = false;
                for (const auto &actual : mirrored->cues) {
                    if (actual.kind == domain::CuePoint::Kind::Hot && actual.hotCueNumber == planned.hotCueNumber
                        && samePosition(actual.positionMs, planned.positionMs)) {
                        landed = true;
                    }
                }
                ++checked;
                check(landed, "the OneLibrary copy of rekordbox track " + target.sourceId + " also has the cue at "
                                  + std::to_string(static_cast<long long>(planned.positionMs))
                                  + " ms -- one library written twice must not disagree with itself");
            }
        }
        int rekordboxTargets = 0;
        for (const auto &plan : withCues) {
            const domain::Track &target =
                plan.direction == domain::SyncPlan::Direction::ToB ? plan.match.trackB : plan.match.trackA;
            if (target.format == "rekordbox") {
                ++rekordboxTargets;
            }
        }
        if (checked > 0) {
            std::cout << "    sync: " << checked << " cue(s) verified in the OneLibrary copy too\n";
        } else {
            // Say so rather than pass quietly: an assertion that examines
            // nothing is indistinguishable from one that holds.
            //
            // On the committed fixture it examines nothing for a reason
            // worth knowing: the anonymizer renamed each catalog's files
            // independently, so the same audio file has a different
            // placeholder name in export.pdb and in exportLibrary.db. The
            // cross-catalog identity the three-format work depends on is
            // exactly what the anonymization destroys, which means no
            // OneLibrary mirror -- not this one, and not the three that
            // predate it -- has ever been verified by this fixture.
            auto basenameOf = [](std::string path) {
                while (!path.empty() && path.back() == ' ') {
                    path.pop_back();
                }
                return fs::path(path).filename().string();
            };
            std::set<std::string> oneLibraryNames;
            for (const auto &t : oneLibraryAfter) {
                oneLibraryNames.insert(basenameOf(t.filePath));
            }
            int shared = 0;
            for (const auto &t : catalogs.rekordbox) {
                if (oneLibraryNames.count(basenameOf(t.filePath))) {
                    ++shared;
                }
            }
            std::cout << "    sync: OneLibrary mirror NOT exercised -- " << rekordboxTargets << " of "
                      << withCues.size() << " plans target rekordbox, but only " << shared << " of "
                      << catalogs.rekordbox.size()
                      << " rekordbox tracks share a filename with any OneLibrary row, so the two catalogs "
                         "cannot be matched up in this data set at all\n";
        }
    }

    expected.expect("matrix.sync.engineOpens", counts.engineDatabaseOpens, "sync Engine opens unchanged");
    expected.expect("matrix.sync.durableWritesPerSave", counts.durableFileWrites,
                    "sync durable whole-file writes for the whole save unchanged");
    std::cout << "    sync (" << withCues.size() << " plans): " << counts.describe() << "\n";
    fs::remove_all(rekordboxRoot);
    fs::remove_all(engineRoot);
    pass("matrix: every planned cue lands and reads back equal");
}

// Matrix: Device settings. The written field reads back, and every other
// byte of the file is untouched -- a settings writer that rewrites the
// whole file would pass a read-back check and still be wrong.
void caseDeviceSettings(const DataSet &set, const fs::path &scratch, Expectations &expected)
{
    if (!set.rekordboxRoot) {
        return;
    }
    const fs::path root = freshRekordboxCopy(set, scratch, "matrix-settings");
    const fs::path settingsFile = root / "MYSETTING.DAT";
    if (!fs::exists(settingsFile)) {
        std::cout << "    skipped matrix/device-settings: this set has no MYSETTING.DAT\n";
        fs::remove_all(root);
        return;
    }

    auto files = infrastructure::rekordbox::readDeviceSettings(root.string());
    const infrastructure::rekordbox::SettingsFile *mySetting = nullptr;
    for (const auto &file : files) {
        if (file.fileName == "MYSETTING.DAT" && !file.fields.empty()) {
            mySetting = &file;
        }
    }
    if (mySetting == nullptr) {
        std::cout << "    skipped matrix/device-settings: nothing recognised in MYSETTING.DAT\n";
        fs::remove_all(root);
        return;
    }

    // A field with at least two options, so there is something to change
    // it to.
    std::string label;
    std::string current;
    std::string wanted;
    for (const auto &[fieldLabel, value] : mySetting->fields) {
        for (const auto &field : infrastructure::rekordbox::allSettingsFields()) {
            if (field.fileName != "MYSETTING.DAT" || field.label != fieldLabel || field.options.size() < 2) {
                continue;
            }
            for (const auto &option : field.options) {
                if (option.name != value) {
                    label = fieldLabel;
                    current = value;
                    wanted = option.name;
                    break;
                }
            }
            break;
        }
        if (!label.empty()) {
            break;
        }
    }
    if (label.empty()) {
        std::cout << "    skipped matrix/device-settings: no field has an alternative value\n";
        fs::remove_all(root);
        return;
    }

    const std::string before = readWholeFile(settingsFile);
    auto change = std::make_shared<gui::DeviceSettingChange>(
        QString::fromStdString(root.string()), "MYSETTING.DAT", QString::fromStdString(label),
        QString::fromStdString(current), QString::fromStdString(wanted));
    WorkCounters::instance().reset();
    auto result = runChanges({change}, root, {});
    const auto counts = WorkCounters::instance().snapshot();
    expected.expect("matrix.deviceSetting.durableWritesPerSave", counts.durableFileWrites,
                    "device-setting durable whole-file writes for the whole save unchanged");
    std::cout << "    device setting: " << counts.describe() << "\n";
    if (!check(result.error.isEmpty(), "the settings save reported no error: " + result.error.toStdString())) {
        fs::remove_all(root);
        return;
    }

    auto after = infrastructure::rekordbox::readDeviceSettings(root.string());
    bool found = false;
    for (const auto &file : after) {
        if (file.fileName != "MYSETTING.DAT") {
            continue;
        }
        for (const auto &[fieldLabel, value] : file.fields) {
            if (fieldLabel != label) {
                continue;
            }
            found = true;
            check(value == wanted, "the settings field reads back as what was written");
        }
    }
    check(found, "the settings field is still present after the save");

    // Byte-for-byte identical apart from what one field occupies: same
    // length, and differing in a small number of bytes rather than being
    // rewritten wholesale.
    const std::string afterBytes = readWholeFile(settingsFile);
    if (check(afterBytes.size() == before.size(), "the settings file kept its exact size")) {
        size_t differing = 0;
        for (size_t i = 0; i < before.size(); ++i) {
            if (before[i] != afterBytes[i]) {
                ++differing;
            }
        }
        // One field plus the checksum the format carries.
        check(differing > 0 && differing <= 8,
              "only the one field changed (" + std::to_string(differing) + " byte(s) differ)");
    }
    fs::remove_all(root);
    pass("matrix: a settings field reads back and nothing else moved");
}


// Matrix: copy cues between duplicates. The destination ends up with the
// source's cues; the source is untouched.
void caseCopyCues(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs, Expectations &expected)
{
    const domain::Track *source = nullptr;
    const domain::Track *target = nullptr;
    for (const auto &t : catalogs.rekordbox) {
        if (source == nullptr && !t.cues.empty()) {
            source = &t;
        } else if (target == nullptr && t.cues.empty()) {
            target = &t;
        }
        if (source && target) {
            break;
        }
    }
    if (!source || !target) {
        std::cout << "    skipped matrix/copy-cues: need one track with cues and one without\n";
        return;
    }
    const std::string sourceId = source->sourceId;
    const std::string targetId = target->sourceId;
    const size_t expectedCues = source->cues.size();
    const fs::path root = freshRekordboxCopy(set, scratch, "matrix-copycues");

    gui::DuplicatesCopyOp op;
    op.source = *source;
    op.targets = {*target};
    auto change = std::make_shared<gui::CopyCuesChange>("rekordbox", QString::fromStdString(root.string()),
                                                        QString::fromStdString(targetId), op);
    WorkCounters::instance().reset();
    auto result = runChanges({change}, root, {});
    const auto counts = WorkCounters::instance().snapshot();
    expected.expect("matrix.copyCues.durableWritesPerSave", counts.durableFileWrites,
                    "copy-cues durable whole-file writes for the whole save unchanged");
    std::cout << "    copy cues: " << counts.describe() << "\n";
    if (!check(result.error.isEmpty(), "the copy-cues save reported no error: " + result.error.toStdString())) {
        fs::remove_all(root);
        return;
    }

    auto after = rescanRekordbox(root);
    const domain::Track *rereadTarget = findTrack(after, targetId);
    const domain::Track *rereadSource = findTrack(after, sourceId);
    if (check(rereadTarget != nullptr, "the destination track survived")) {
        check(rereadTarget->cues.size() == expectedCues, "the destination has the source's cue count");
        for (const auto &wanted : op.source.cues) {
            bool landed = false;
            for (const auto &actual : rereadTarget->cues) {
                if (actual.kind == wanted.kind && actual.hotCueNumber == wanted.hotCueNumber
                    && actual.positionMs == wanted.positionMs) {
                    landed = true;
                }
            }
            check(landed, "a copied cue landed at its own position on the destination");
        }
    }
    if (check(rereadSource != nullptr, "the source track survived")) {
        check(rereadSource->cues.size() == expectedCues, "the source kept exactly its own cues");
    }
    fs::remove_all(root);
    pass("matrix: cues copy onto the other copy and the source is untouched");
}

// Matrix: local cue restore. The merged set is what the candidate carried,
// which is the stick's own cues plus whatever the backup filled in --
// never fewer, since a cue writer replaces the whole set.
void caseLocalCueRestore(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs, Expectations &expected)
{
    const domain::Track *target = nullptr;
    for (const auto &t : catalogs.rekordbox) {
        if (t.cues.empty()) {
            target = &t;
            break;
        }
    }
    if (target == nullptr) {
        std::cout << "    skipped matrix/local-restore: no track without cues\n";
        return;
    }
    const std::string id = target->sourceId;
    const fs::path root = freshRekordboxCopy(set, scratch, "matrix-localcue");

    domain::RestoreCandidate candidate;
    candidate.stickTrack = *target;
    candidate.localTrack = *target;
    domain::CuePoint restored;
    restored.kind = domain::CuePoint::Kind::Hot;
    restored.hotCueNumber = 3;
    restored.positionMs = 21000.0;
    candidate.mergedCues = {restored};

    auto change = std::make_shared<gui::MergeCuesChange>("rekordbox", QString::fromStdString(root.string()), candidate);
    WorkCounters::instance().reset();
    auto result = runChanges({change}, root, {});
    const auto counts = WorkCounters::instance().snapshot();
    expected.expect("matrix.mergeCues.durableWritesPerSave", counts.durableFileWrites,
                    "merge-cues durable whole-file writes for the whole save unchanged");
    std::cout << "    merge cues: " << counts.describe() << "\n";
    if (!check(result.error.isEmpty(), "the local-restore save reported no error: " + result.error.toStdString())) {
        fs::remove_all(root);
        return;
    }

    auto after = rescanRekordbox(root);
    const domain::Track *reread = findTrack(after, id);
    if (check(reread != nullptr, "the restored track survived")) {
        if (check(reread->cues.size() == candidate.mergedCues.size(), "the track has exactly the merged cue set")) {
            check(reread->cues[0].hotCueNumber == 3, "the restored cue kept its slot");
            check(reread->cues[0].positionMs == 21000.0, "the restored cue kept its position");
        }
    }
    fs::remove_all(root);
    pass("matrix: a restored cue reads back as what was merged");
}

// Matrix: Library Health repair. The broken row's cues merge onto the
// survivor, the broken row goes, and the survivor keeps its playlists.
void caseLibraryHealthRepair(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs,
                             Expectations &expected)
{
    if (catalogs.rekordbox.size() < 30) {
        std::cout << "    skipped matrix/repair: too few rekordbox tracks\n";
        return;
    }
    const fs::path root = freshRekordboxCopy(set, scratch, "matrix-repair");
    auto tracks = rescanRekordbox(root);
    const size_t before = tracks.size();

    // A survivor that has playlist membership, so the repair has real
    // repointing to do, and a broken row carrying cues to merge.
    const domain::Track *survivor = nullptr;
    for (const auto &t : tracks) {
        if (!t.playlists.empty()) {
            survivor = &t;
            break;
        }
    }
    const domain::Track *broken = nullptr;
    for (const auto &t : tracks) {
        if (survivor && t.sourceId != survivor->sourceId && !t.cues.empty()) {
            broken = &t;
            break;
        }
    }
    if (!survivor || !broken) {
        std::cout << "    skipped matrix/repair: need a survivor with playlists and a broken row with cues\n";
        fs::remove_all(root);
        return;
    }
    const std::string survivorId = survivor->sourceId;
    const std::string brokenId = broken->sourceId;
    const size_t survivorPlaylists = survivor->playlists.size();

    domain::LibraryConsistencyIssue issue;
    issue.kind = domain::LibraryConsistencyIssue::Kind::Repairable;
    issue.survivor = *survivor;
    issue.brokenGroup = {*broken};
    issue.survivorCues = broken->cues;

    WorkCounters::instance().reset();
    auto change = std::make_shared<gui::RepairIssueChange>(QString::fromStdString(root.string()), issue, 1);
    auto result = runChanges({change}, root, {});
    const auto counts = WorkCounters::instance().snapshot();
    if (!check(result.error.isEmpty(), "the repair save reported no error: " + result.error.toStdString())) {
        fs::remove_all(root);
        return;
    }

    auto after = rescanRekordbox(root);
    check(after.size() == before - 1, "the broken row is gone");
    check(findTrack(after, brokenId) == nullptr, "the broken row is really gone");
    const domain::Track *repaired = findTrack(after, survivorId);
    if (check(repaired != nullptr, "the survivor is still there")) {
        // The survivor must resolve to something a later scan can find,
        // and must not have lost the playlists it was in.
        check(!repaired->filePath.empty(), "the survivor still names a file");
        check(repaired->playlists.size() >= survivorPlaylists, "the survivor kept its playlist membership");
        for (const auto &wanted : issue.survivorCues) {
            bool landed = false;
            for (const auto &actual : repaired->cues) {
                if (actual.kind == wanted.kind && actual.hotCueNumber == wanted.hotCueNumber
                    && actual.positionMs == wanted.positionMs) {
                    landed = true;
                }
            }
            check(landed, "a merged cue landed on the survivor");
        }
    }
    expected.expect("matrix.repair.pdbParses", counts.trackDatabaseParses, "repair pdb parses unchanged");
    expected.expect("matrix.repair.durableWritesPerSave", counts.durableFileWrites,
                    "repair durable whole-file writes for the whole save unchanged");
    std::cout << "    library health repair: " << counts.describe() << "\n";
    fs::remove_all(root);
    pass("matrix: a repair merges cues onto the survivor and removes the broken row");
}

// Matrix: a cleanup that has to write TWO catalogs. The whole point of
// treating a file rather than a row as the unit of duplication is that one
// file is listed by rekordbox and by Engine at once, and removing it means
// removing both rows -- with each catalog's survivor row given the merged
// cues BEFORE its doomed row goes, or the cues that only lived on the
// doomed row are lost in that catalog.
//
// Built so it cannot pass unless the second catalog was really written.
// The decisive assertions are the Engine ones: the Engine row gone from a
// fresh Engine scan, and the merged cues present on the Engine survivor.
// Nothing in the single-catalog path touches either, so a save that
// quietly wrote only rekordbox fails here rather than looking correct --
// which matters, because that is precisely the state this code was in
// before, and 105 green tests said nothing about it.
void caseCleanUpAcrossCatalogs(const DataSet &set, const fs::path &scratch)
{
    if (!set.rekordboxRoot || !set.engineRoot) {
        std::cout << "    skipped matrix/cross-catalog cleanup: this set has only one catalog\n";
        return;
    }

    // A real stick's layout, not the set's own: catalogPathFor() finds the
    // sibling catalog by walking up from one of them, so the two have to
    // sit under a single root named the way a stick names them. That is
    // also what makes this test exercise the path resolution rather than
    // being handed both paths.
    const fs::path stick = scratch / "matrix-xcat";
    fs::remove_all(stick);
    fs::create_directories(stick);
    fs::copy(*set.rekordboxRoot, stick / "PIONEER", fs::copy_options::recursive);
    fs::copy(*set.engineRoot, stick / "Engine Library", fs::copy_options::recursive);
    const fs::path pioneer = stick / "PIONEER";
    const fs::path engineLib = stick / "Engine Library";

    auto rekordboxBefore = rescanRekordbox(pioneer);
    auto engineBefore = rescanEngine(engineLib);

    std::vector<domain::Track> rows = rekordboxBefore;
    rows.insert(rows.end(), engineBefore.begin(), engineBefore.end());
    const auto files = application::collapseCatalogRows(rows);

    auto bothFormats = [](const domain::Track &file) {
        bool rekordbox = false, engine = false;
        for (const auto &row : file.catalogRows) {
            rekordbox = rekordbox || row.format == "rekordbox";
            engine = engine || row.format == "engine";
        }
        return rekordbox && engine;
    };
    // Chosen by the real planner rather than by hand. Picking a survivor
    // with no cues (as the single-catalog case does) cannot work here: a
    // collapsed file's cues are the union of its rows', so on real data
    // every one of the 1161 cross-format files has some. Running the
    // planner also means this exercises survivor selection and the
    // stranding check rather than assuming them.
    std::vector<const domain::Track *> crossFormat;
    for (const auto &file : files) {
        if (bothFormats(file)) {
            crossFormat.push_back(&file);
        }
    }

    domain::DuplicateCleanupPlan plan;
    bool found = false;
    // Bounded: a pair is normally found within the first few, and this is
    // a matrix case that runs on every corpus set.
    const std::size_t limit = std::min<std::size_t>(crossFormat.size(), 60);
    for (std::size_t i = 0; i < limit && !found; ++i) {
        for (std::size_t j = 0; j < limit && !found; ++j) {
            if (i == j) {
                continue;
            }
            domain::DuplicateGroup group;
            group.tracks = {*crossFormat[i], *crossFormat[j]};
            auto candidate = domain::DuplicateCleanupPlanner::plan(group);
            // Needs a cue the survivor does not already have, or the save
            // writes no cues and the Engine cue assertion below would pass
            // against data that was already correct -- the exact shape of
            // check this suite has been finding all day.
            if (candidate.mergedCuesForSurvivor.size() > candidate.survivor.cues.size()
                && !candidate.wouldStrandAFormat && !candidate.toRemove.empty()
                && !candidate.toRemove[0].isUnreferenced) {
                plan = candidate;
                found = true;
            }
        }
    }
    if (!found) {
        std::cout << "    (no cross-catalog cleanup to check: this set has no pair of files listed by "
                     "both catalogs where one holds a cue the other lacks)\n";
        fs::remove_all(stick);
        return;
    }
    const domain::Track *survivor = &plan.survivor;
    const domain::Track *doomed = &plan.toRemove[0];

    auto rowIdIn = [](const domain::Track &track, const std::string &format) {
        for (const auto &row : track.catalogRows) {
            if (row.format == format) {
                return row.sourceId;
            }
        }
        return std::string();
    };
    const std::string doomedEngineId = rowIdIn(*doomed, "engine");
    const std::string survivorEngineId = rowIdIn(*survivor, "engine");
    const std::string doomedRekordboxId = rowIdIn(*doomed, "rekordbox");

    auto change = std::make_shared<gui::CleanupGroupChange>("rekordbox", QString::fromStdString(pioneer.string()),
                                                            plan, 1);
    auto result = runChanges({change}, pioneer, engineLib);
    if (!check(result.error.isEmpty(),
               "the cross-catalog cleanup save reported no error: " + result.error.toStdString())) {
        fs::remove_all(stick);
        return;
    }

    auto rekordboxAfter = rescanRekordbox(pioneer);
    auto engineAfter = rescanEngine(engineLib);

    check(findTrack(rekordboxAfter, doomedRekordboxId) == nullptr,
          "the doomed copy's rekordbox row is gone");

    // The four that only the second catalog's write can satisfy.
    check(engineAfter.size() == engineBefore.size() - 1,
          "the Engine catalog lost exactly one row");
    check(findTrack(engineAfter, doomedEngineId) == nullptr,
          "the doomed copy's ENGINE row is gone, not just its rekordbox one");
    const domain::Track *engineSurvivor = findTrack(engineAfter, survivorEngineId);
    if (check(engineSurvivor != nullptr, "the survivor's Engine row is still there")) {
        for (const auto &wanted : plan.mergedCuesForSurvivor) {
            bool landed = false;
            for (const auto &actual : engineSurvivor->cues) {
                if (actual.kind == wanted.kind && actual.hotCueNumber == wanted.hotCueNumber
                    && actual.positionMs == wanted.positionMs) {
                    landed = true;
                }
            }
            check(landed, "a cue that only existed on the removed copy survived onto the ENGINE survivor");
        }
    }
    std::cout << "    cross-catalog cleanup: rekordbox " << rekordboxBefore.size() << "->"
              << rekordboxAfter.size() << ", engine " << engineBefore.size() << "->" << engineAfter.size()
              << ", " << plan.mergedCuesForSurvivor.size() << " cue(s) preserved in both\n";
    fs::remove_all(stick);
}

// Matrix: Clean Up duplicates. The survivor ends up with the union of the
// group's cues, every removed row is gone from a fresh scan, and each
// removed copy is named in the pending-deletion manifest rather than
// deleted here.
void caseCleanUpDuplicates(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs,
                           Expectations &expected)
{
    if (catalogs.rekordbox.size() < 30) {
        std::cout << "    skipped matrix/cleanup: too few rekordbox tracks\n";
        return;
    }
    const fs::path root = freshRekordboxCopy(set, scratch, "matrix-cleanup");
    auto tracks = rescanRekordbox(root);
    const size_t before = tracks.size();

    const domain::Track *survivor = nullptr;
    const domain::Track *doomed = nullptr;
    for (const auto &t : tracks) {
        if (survivor == nullptr && t.cues.empty()) {
            survivor = &t;
        } else if (doomed == nullptr && !t.cues.empty()) {
            doomed = &t;
        }
        if (survivor && doomed) {
            break;
        }
    }
    if (!survivor || !doomed) {
        std::cout << "    skipped matrix/cleanup: need a survivor without cues and a doomed copy with them\n";
        fs::remove_all(root);
        return;
    }
    const std::string survivorId = survivor->sourceId;
    const std::string doomedId = doomed->sourceId;

    domain::DuplicateCleanupPlan plan;
    plan.group.tracks = {*survivor, *doomed};
    plan.survivor = *survivor;
    plan.toRemove = {*doomed};
    // The union: the survivor has none, so the doomed copy's cues are what
    // must survive it. Losing these is exactly what this feature must never
    // do.
    plan.mergedCuesForSurvivor = doomed->cues;

    WorkCounters::instance().reset();
    auto change = std::make_shared<gui::CleanupGroupChange>("rekordbox", QString::fromStdString(root.string()), plan, 1);
    auto result = runChanges({change}, root, {});
    const auto counts = WorkCounters::instance().snapshot();
    if (!check(result.error.isEmpty(), "the cleanup save reported no error: " + result.error.toStdString())) {
        fs::remove_all(root);
        return;
    }

    auto after = rescanRekordbox(root);
    check(after.size() == before - 1, "the doomed copy's row is gone");
    check(findTrack(after, doomedId) == nullptr, "the doomed copy is really gone");
    const domain::Track *kept = findTrack(after, survivorId);
    if (check(kept != nullptr, "the survivor is still there")) {
        for (const auto &wanted : plan.mergedCuesForSurvivor) {
            bool landed = false;
            for (const auto &actual : kept->cues) {
                if (actual.kind == wanted.kind && actual.hotCueNumber == wanted.hotCueNumber
                    && actual.positionMs == wanted.positionMs) {
                    landed = true;
                }
            }
            check(landed, "a cue that only existed on the removed copy survived onto the survivor");
        }
    }

    // The removed copy is scheduled, not deleted: the file must still be
    // there and the manifest must name it.
    const fs::path manifestPath = scratch / "Seabass" / "orphaned" / "pending-deletions.jsonl";
    if (check(fs::exists(manifestPath), "a pending-deletion manifest was written")) {
        infrastructure::cleanup::PendingDeletionManifest manifest(manifestPath.string());
        auto entries = manifest.list();
        bool named = false;
        for (const auto &entry : entries) {
            if (entry.filePath == doomed->filePath) {
                named = true;
            }
        }
        check(named, "the removed copy's file is named in the pending-deletion manifest");
    }
    expected.expect("matrix.cleanup.pdbParses", counts.trackDatabaseParses, "cleanup pdb parses unchanged");
    expected.expect("matrix.cleanup.durableWritesPerSave", counts.durableFileWrites,
                    "cleanup durable whole-file writes for the whole save unchanged");
    std::cout << "    clean up duplicates: " << counts.describe() << "\n";
    fs::remove_all(root);
    fs::remove(manifestPath);
    pass("matrix: cleanup keeps every cue, removes the row, and schedules the file");
}

// Matrix: delete orphaned OneLibrary rows. OneLibrary only, because it is
// the one catalog whose rows are deleted outright rather than repointed at
// a survivor.
void caseDeleteOrphan(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs, Expectations &expected)
{
    if (catalogs.oneLibrary.empty()) {
        std::cout << "    skipped matrix/delete-orphan: this set has no OneLibrary database\n";
        return;
    }
    const fs::path root = freshRekordboxCopy(set, scratch, "matrix-orphan");
    std::vector<domain::Track> tracks;
    try {
        infrastructure::onelibrary::OneLibraryReader reader(root.string());
        tracks = reader.readAll();
    } catch (const std::exception &e) {
        check(false, std::string("could not read OneLibrary from the scratch copy: ") + e.what());
        fs::remove_all(root);
        return;
    }
    if (tracks.empty()) {
        std::cout << "    skipped matrix/delete-orphan: the scratch OneLibrary came back empty\n";
        fs::remove_all(root);
        return;
    }
    const size_t before = tracks.size();
    const domain::Track doomed = tracks.front();

    domain::LibraryConsistencyIssue issue;
    issue.kind = domain::LibraryConsistencyIssue::Kind::Missing;
    issue.brokenGroup = {doomed};

    auto change = std::make_shared<gui::DeleteOrphanChange>(QString::fromStdString(root.string()), issue);
    WorkCounters::instance().reset();
    auto result = runChanges({change}, root, {});
    const auto counts = WorkCounters::instance().snapshot();
    expected.expect("matrix.deleteOrphan.durableWritesPerSave", counts.durableFileWrites,
                    "delete-orphan durable whole-file writes for the whole save unchanged");
    std::cout << "    delete orphan: " << counts.describe() << "\n";
    if (!check(result.error.isEmpty(), "the orphan-deletion save reported no error: " + result.error.toStdString())) {
        fs::remove_all(root);
        return;
    }

    std::vector<domain::Track> after;
    try {
        infrastructure::onelibrary::OneLibraryReader reader(root.string());
        after = reader.readAll();
    } catch (const std::exception &e) {
        check(false, std::string("could not re-read OneLibrary after the deletion: ") + e.what());
        fs::remove_all(root);
        return;
    }
    check(after.size() == before - 1, "exactly one OneLibrary row went");
    check(findTrack(after, doomed.sourceId) == nullptr, "the orphaned row is really gone");
    fs::remove_all(root);
    pass("matrix: an orphaned OneLibrary row is deleted and nothing else with it");
}

#endif  // SEABASS_CORPUS_HAS_EDIT

namespace
{

void runMatrix(const DataSet &set, const fs::path &scratch, const Catalogs &catalogs, Expectations &expected)
{
#ifdef SEABASS_CORPUS_HAS_EDIT
    std::cout << "  matrix: one editing feature per case, staged through the real save loop\n";
    caseAddCue(set, scratch, catalogs, expected);
    caseStrayCueRemoval(set, scratch, catalogs, expected);
    caseBackupPrecedesWrites(set, scratch, catalogs);
    caseNoPaddedStringsFromRekordbox(set, catalogs);
    caseCollapseGroupsAcrossFormats(set, catalogs);
    caseCleanUpAcrossCatalogs(set, scratch);
    caseBackupPathResolver(set, scratch, catalogs);
    caseBackupCoversEveryChangedFile(set, scratch, catalogs);
    caseSync(set, scratch, catalogs, expected);
    caseDeviceSettings(set, scratch, expected);
    caseCopyCues(set, scratch, catalogs, expected);
    caseLocalCueRestore(set, scratch, catalogs, expected);
    caseLibraryHealthRepair(set, scratch, catalogs, expected);
    caseCleanUpDuplicates(set, scratch, catalogs, expected);
    caseDeleteOrphan(set, scratch, catalogs, expected);
#else
    (void)set;
    (void)scratch;
    (void)catalogs;
    (void)expected;
    std::cout << "  matrix: skipped, this build has no Qt so the change classes are not available\n";
#endif
}

}  // namespace

int main()
{
    auto sets = discoverSets();
    if (sets.empty()) {
        std::cout << "no data sets found\n";
        return 1;
    }
    std::cout << "corpus: " << sets.size() << " set(s)\n";

    for (const auto &set : sets) {
        g_set = set.name;
        std::cout << "\n== " << set.name << (set.anonymized ? " (anonymized)" : " (real)") << " ==\n";
        const fs::path scratch = scratchFor(set.name);
        Expectations expected(set.expectationsPath);
        Catalogs catalogs;

        std::cout << "  integrity\n";
        caseScanCounts(set, catalogs, expected);
        caseStatistics(catalogs);
        caseSyncMatching(set, catalogs, expected);
        casePlaceholderCollisions(set, catalogs);
        caseConsistencyChecker(catalogs);
        if (set.rekordboxRoot) {
            caseCueRoundTrip(set, scratch, catalogs);
            casePendingDeletion(set, scratch, catalogs);
            caseInterruptedBatch(set, scratch, catalogs);
        }

        std::cout << "  stability\n";
        caseEngineStability(set, scratch, catalogs, 50, expected);

        std::cout << "  work counts\n";
        caseWorkCounts(set, scratch, catalogs, 20, expected);

        runMatrix(set, scratch, catalogs, expected);

        expected.save();
        fs::remove_all(scratch);
    }

    std::error_code ec;
    fs::remove_all(unpackedSetsRoot(), ec);

    std::cout << "\n";
    if (g_failures > 0) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
