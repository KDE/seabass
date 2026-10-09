// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: Sync after Rekordbox Export on a test stick, headless -- the
// rig half of step 13 of docs/sync-after-rekordbox-export-plan.md, run by
// check W10 in tools/rig-edits.sh.
//
//   rig_engine_update --rekordbox P --engine E <subcommand> [argument]
//
// P and E are one stick's PIONEER and Engine Library folders; the stick
// root is their parent.
//
//   --pick
//       prints the stick-relative path of a track the check can plant:
//       one rekordbox row whose file is on the stick, one Engine row for
//       the same file, and at least one rekordbox playlist, every one of
//       which Engine has at that path. Reads only.
//   --plant-baseline-without <stick-relative path>
//       makes the stick read as "rekordbox added this track since the
//       last save": writes a baseline equal to the rekordbox side now
//       minus that track and its memberships, and removes the track's
//       Engine row with every row naming it (removeEngineTrackRows).
//       Engine's import counter is levelled with export.pdb first, as
//       any earlier save with both catalogs leaves it, so the save and
//       its undo can be byte for byte. m.db and any baseline already on
//       the stick are copied to <stick>/RIG-ENGINE-UPDATE/ before
//       anything changes, for --restore-plant.
//   --plan
//       the proposal, as seabass-cli sync-after-export counts it, and the
//       tracks it would add. Reads only (Engine's cues at each file's own
//       rate, nothing cached). Exit 0 nothing to do, 1 something, 2 error.
//   --sync
//       what the page does: analyze through RekordboxExportSyncController,
//       keep the rows ticked by default, stageSelected(), save through the
//       stick's session; prints the summary and the analysis after it.
//       The save's backup records go to RIG-ENGINE-UPDATE/last-save.tsv.
//       Exit 0 when the analysis after the save has no ticked row.
//   --undo
//       the undo of that save. A finished save's undo lives in the
//       session's memory (LibraryEditSession::undoLastSave stages a
//       RestoreBackupsChange over the save's records), and this is a new
//       process, so it stages the same change over the records --sync
//       wrote down, saves, and prints the analysis that comes back.
//   --check-back <stick-relative path>
//       after --sync: exactly one Engine row for the file, in every
//       rekordbox playlist of the track that Engine has. Reads only.
//   --catalog-shas <file> / --catalog-compare <file>
//       rig_catalog.hpp's catalog files with their sha256, written down /
//       compared byte for byte, a file added or gone included.
//   --restore-plant
//       puts m.db and the stick's baseline back as --plant found them and
//       removes RIG-ENGINE-UPDATE.
//
// Every subcommand refuses a protected stick (rig_catalog.hpp's
// isProtectedStick: WHALESHARK, WHALESHARK2, CORSAIR) before reading it.
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL" (--plan says
// what it found instead), and the exit code matches.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "application/ports/backup_store.hpp"
#include "application/use_cases/fill_file_sizes.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "domain/engine_update_planning.hpp"
#include "domain/rekordbox_baseline.hpp"
#include "gui/edit/changes/restore_backups_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/rekordbox_baseline_ledger.hpp"
#include "gui/rekordbox_export_sync_controller.hpp"
#include "gui/rekordbox_export_sync_list_model.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/local/engine_update_stick_facts.hpp"
#include "infrastructure/local/rekordbox_baseline_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "rig_catalog.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using gui::RekordboxExportSyncController;
using gui::RekordboxExportSyncListModel;

namespace
{

constexpr int Pass = 0;
constexpr int Fail = 1;
constexpr int Error = 2;

int result(bool passed)
{
    std::cout << "RIG RESULT: " << (passed ? "PASS" : "FAIL") << "\n";
    return passed ? Pass : Fail;
}

struct Stick
{
    fs::path pioneer;
    fs::path engine;
    fs::path root;

    std::string pioneerUtf8() const { return pathToUtf8(pioneer); }
    std::string engineUtf8() const { return pathToUtf8(engine); }
    std::string rootUtf8() const { return pathToUtf8(root); }
    fs::path mdb() const { return engine / "Database2" / "m.db"; }
    fs::path rigDir() const { return root / "RIG-ENGINE-UPDATE"; }
    fs::path baselineFile() const { return infrastructure::paths::stickRekordboxBaseline(root); }
};

// Absolute and without a trailing separator, as seabass-cli makes them,
// so the stick root is the catalogs' parent and every Track::filePath
// lies under it.
fs::path catalogDir(const std::string &given)
{
    fs::path path = fs::absolute(pathFromUtf8(given)).lexically_normal();
    if (path.filename().empty() && path.has_parent_path()) {
        path = path.parent_path();
    }
    return path;
}

// The key a stick-relative path is planned and recorded under.
std::string keyOfRelative(const std::string &relative)
{
    return gui::baselinePathKey(relative);
}

std::string relativeOf(const Stick &stick, const domain::Track &track)
{
    return gui::baselineStickRelativePath(stick.rootUtf8(), track.filePath);
}

std::vector<domain::Track> readRekordbox(const Stick &stick)
{
    infrastructure::rekordbox::KaitaiRekordboxReader reader(stick.pioneerUtf8());
    return reader.readAll();
}

// Engine's rows without their cues' sample rates looked up: only paths,
// ids and memberships are asked of them here.
std::vector<domain::Track> readEngineRows(const Stick &stick)
{
    infrastructure::engine::LibdjinteropEngineReader reader(stick.engineUtf8());
    std::vector<domain::Track> rows;
    for (auto &track : reader.readTracks()) {
        if (track.streamingSource.empty()) {
            rows.push_back(std::move(track));
        }
    }
    return rows;
}

std::vector<const domain::Track *> rowsWithKey(const Stick &stick, const std::vector<domain::Track> &tracks,
                                               const std::string &key)
{
    std::vector<const domain::Track *> out;
    for (const auto &track : tracks) {
        if (keyOfRelative(relativeOf(stick, track)) == key) {
            out.push_back(&track);
        }
    }
    return out;
}

std::set<std::string> playlistNames(const domain::Track &track)
{
    std::set<std::string> names;
    for (const auto &membership : track.playlists) {
        names.insert(membership.name);
    }
    return names;
}

// Engine's playlists (not folders) that sit alone at their path: the ones
// a membership add can land in.
std::set<std::string> engineListPaths(const Stick &stick)
{
    const auto facts = infrastructure::local::readEngineUpdateStickFacts(stick.pioneerUtf8(), stick.engineUtf8());
    std::set<std::string> paths;
    for (const auto &playlist : facts.enginePlaylists) {
        if (!playlist.folder && playlist.countAtPath == 1) {
            paths.insert(playlist.path);
        }
    }
    return paths;
}

std::string join(const std::set<std::string> &names)
{
    std::string out;
    for (const auto &name : names) {
        out += (out.empty() ? "\"" : ", \"") + name + "\"";
    }
    return out.empty() ? std::string("none") : out;
}

// ---- --pick -----------------------------------------------------------

int pick(const Stick &stick)
{
    const std::vector<domain::Track> rekordbox = readRekordbox(stick);
    const std::vector<domain::Track> engine = readEngineRows(stick);
    const std::set<std::string> engineLists = engineListPaths(stick);
    std::map<std::string, int> rekordboxRowsByKey;
    std::map<std::string, int> engineRowsByKey;
    for (const auto &track : rekordbox) {
        ++rekordboxRowsByKey[keyOfRelative(relativeOf(stick, track))];
    }
    for (const auto &track : engine) {
        ++engineRowsByKey[keyOfRelative(relativeOf(stick, track))];
    }
    std::size_t noFile = 0;
    std::size_t notOneEngineRow = 0;
    std::size_t noPlaylist = 0;
    std::size_t playlistNotOnEngine = 0;
    for (const auto &track : rekordbox) {
        const std::string relative = relativeOf(stick, track);
        const std::string key = keyOfRelative(relative);
        if (key.empty() || rekordboxRowsByKey[key] != 1) {
            continue;
        }
        std::error_code ec;
        if (!fs::is_regular_file(pathFromUtf8(track.filePath), ec)) {
            ++noFile;
            continue;
        }
        if (engineRowsByKey[key] != 1) {
            ++notOneEngineRow;
            continue;
        }
        const std::set<std::string> lists = playlistNames(track);
        if (lists.empty()) {
            ++noPlaylist;
            continue;
        }
        if (!std::all_of(lists.begin(), lists.end(), [&](const std::string &name) { return engineLists.count(name); })) {
            ++playlistNotOnEngine;
            continue;
        }
        std::cout << relative << "\n";
        std::cerr << "picked \"" << track.artist << " - " << track.title << "\", in " << join(lists) << "\n";
        return Pass;
    }
    std::cerr << "no track to plant among " << rekordbox.size() << " rekordbox rows: " << noFile
              << " without their file on the stick, " << notOneEngineRow << " without exactly one Engine row, "
              << noPlaylist << " in no playlist, " << playlistNotOnEngine
              << " in a playlist Engine does not have at that path\n";
    return Fail;
}

// ---- --plant-baseline-without / --restore-plant -------------------------

const char *const PlantRecord = "planted.tsv";

bool copyOver(const fs::path &from, const fs::path &to, std::string *error)
{
    std::error_code ec;
    const fs::path temporary = to.parent_path() / (to.filename().native() + fs::path(".rig-copy").native());
    fs::copy_file(from, temporary, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        fs::rename(temporary, to, ec);
    }
    if (ec) {
        *error = "could not copy " + pathToUtf8(from) + " to " + pathToUtf8(to) + ": " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

int plant(const Stick &stick, const std::string &relativeArg)
{
    const fs::path record = stick.rigDir() / PlantRecord;
    if (fs::exists(record)) {
        std::cout << "already planted (" << pathToUtf8(record) << "); run --restore-plant first\n";
        return result(false);
    }
    for (const char *suffix : {"-wal", "-journal"}) {
        if (fs::exists(fs::path(stick.mdb().native() + fs::path(suffix).native()))) {
            std::cout << "m.db has a " << suffix << " file beside it: a save did not finish on this stick. Nothing "
                      << "was planted.\n";
            return result(false);
        }
    }
    const std::string key = keyOfRelative(relativeArg);
    if (key.empty()) {
        std::cout << "\"" << relativeArg << "\" is no stick-relative path\n";
        return result(false);
    }

    // Reads first, everything checked, before the stick changes.
    application::EngineUpdateStickFacts facts =
        infrastructure::local::readEngineUpdateStickFacts(stick.pioneerUtf8(), stick.engineUtf8());
    const bool hadBaseline = fs::exists(stick.baselineFile());
    std::vector<domain::Track> rekordbox = readRekordbox(stick);
    const std::vector<domain::Track> engine = readEngineRows(stick);
    const auto rekordboxRows = rowsWithKey(stick, rekordbox, key);
    const auto engineRows = rowsWithKey(stick, engine, key);
    if (rekordboxRows.size() != 1 || engineRows.size() != 1) {
        std::cout << "\"" << relativeArg << "\": " << rekordboxRows.size() << " rekordbox row(s) and "
                  << engineRows.size() << " Engine row(s); the plant needs one of each\n";
        return result(false);
    }
    const domain::Track target = *rekordboxRows.front();
    const domain::Track engineRow = *engineRows.front();
    std::error_code ec;
    if (!fs::is_regular_file(pathFromUtf8(target.filePath), ec)) {
        std::cout << "the track's file is not on the stick (" << target.filePath
                  << "): the page would list it as not added\n";
        return result(false);
    }
    const std::int64_t engineId = std::stoll(engineRow.sourceId);
    std::string error;
    const int rowsNaming = infrastructure::engine::engineRowsNamingTrack(pathToUtf8(stick.mdb()), engineId, &error);
    if (rowsNaming < 0) {
        std::cout << "could not count the Engine rows naming track " << engineId << ": " << error << "\n";
        return result(false);
    }
    const std::uint64_t sequence = facts.currentSequence;

    // The baseline: the rekordbox side now, without the track. Its
    // memberships go with it, since baselineFrom takes them from the
    // tracks it is given.
    std::vector<domain::Track> others;
    for (const auto &track : rekordbox) {
        if (keyOfRelative(relativeOf(stick, track)) != key) {
            others.push_back(track);
        }
    }
    domain::BaselineGaps gaps;
    const std::string root = stick.rootUtf8();
    domain::RekordboxBaseline baseline = domain::baselineFrom(
        others, facts.rekordboxPlaylists, sequence,
        [&root](const std::string &filePath) { return gui::baselineStickRelativePath(root, filePath); },
        [](const std::string &relative) { return gui::baselinePathKey(relative); }, &gaps);
    baseline.recordedAtUnix =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    baseline.writer = "rig_engine_update";

    // What --restore-plant puts back, before anything changes.
    fs::create_directories(stick.rigDir());
    if (!copyOver(stick.mdb(), stick.rigDir() / "m.db", &error)
        || (hadBaseline && !copyOver(stick.baselineFile(), stick.rigDir() / "baseline", &error))) {
        std::cout << error << "\nnothing was planted\n";
        fs::remove_all(stick.rigDir(), ec);
        return result(false);
    }
    const auto importBefore = infrastructure::engine::readRekordboxImportState(stick.engineUtf8(), stick.pioneerUtf8());
    {
        std::ofstream out(record, std::ios::binary);
        out << "track\t" << relativeArg << "\n"
            << "engineId\t" << engineId << "\n"
            << "hadBaseline\t" << (hadBaseline ? 1 : 0) << "\n"
            << "importCounter\t" << importBefore.engineCounter << "\n"
            << "pdbSequence\t" << sequence << "\n";
        if (!out) {
            std::cout << "could not write " << pathToUtf8(record) << "\nnothing was planted\n";
            fs::remove_all(stick.rigDir(), ec);
            return result(false);
        }
    }

    std::cout << "planting on " << root << ":\n"
              << "  track: " << relativeArg << "  \"" << target.artist << " - " << target.title << "\"\n"
              << "  rekordbox playlists: " << join(playlistNames(target)) << "\n"
              << "  Engine row " << engineId << " in " << join(playlistNames(engineRow)) << ", " << rowsNaming
              << " row(s) naming it\n";
    if (importBefore.engineCounter != sequence) {
        if (!infrastructure::engine::markRekordboxLibraryImported(stick.engineUtf8(), sequence, &error)) {
            std::cout << "could not level Engine's import counter: " << error << "\n";
            return result(false);
        }
        std::cout << "  Engine's import counter levelled: " << importBefore.engineCounter << " -> " << sequence << "\n";
    } else {
        std::cout << "  Engine's import counter already level at " << sequence << "\n";
    }
    const int removed = infrastructure::engine::removeEngineTrackRows(pathToUtf8(stick.mdb()), {engineId}, &error);
    if (removed != 1) {
        std::cout << "could not remove Engine row " << engineId << ": " << error << "\n";
        return result(false);
    }
    const int left = infrastructure::engine::engineRowsNamingTrack(pathToUtf8(stick.mdb()), engineId, &error);
    std::cout << "  Engine row " << engineId << " removed, " << left << " row(s) naming it left\n";
    if (!infrastructure::local::writeRekordboxBaseline(stick.root, baseline, [](const std::string &) {}, &error)) {
        std::cout << "could not write the baseline: " << error << "\n";
        return result(false);
    }
    std::cout << "  baseline at export " << sequence << ": " << baseline.tracks.size() << " track(s), "
              << baseline.playlists.size() << " playlist(s) and folder(s), the track left out"
              << (hadBaseline ? " (the stick's own record kept aside)" : "") << "\n";
    if (!gaps.unkeyedTracks.empty() || !gaps.unknownMemberships.empty() || !gaps.orphanPlaylists.empty()) {
        std::cout << "  baseline gaps: " << gaps.unkeyedTracks.size() << " unkeyed track(s), "
                  << gaps.unknownMemberships.size() << " unknown membership(s), " << gaps.orphanPlaylists.size()
                  << " orphan playlist(s)\n";
    }
    return result(left == 0);
}

std::map<std::string, std::string> readRecord(const fs::path &file)
{
    std::map<std::string, std::string> values;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            values[line.substr(0, tab)] = line.substr(tab + 1);
        }
    }
    return values;
}

int restorePlant(const Stick &stick)
{
    const fs::path record = stick.rigDir() / PlantRecord;
    if (!fs::exists(record)) {
        std::cout << "nothing planted on this stick\n";
        return result(true);
    }
    const auto values = readRecord(record);
    std::string error;
    bool ok = copyOver(stick.rigDir() / "m.db", stick.mdb(), &error);
    if (ok) {
        std::cout << "m.db put back\n";
    }
    if (ok && values.count("hadBaseline") && values.at("hadBaseline") == "1") {
        ok = copyOver(stick.rigDir() / "baseline", stick.baselineFile(), &error);
        if (ok) {
            std::cout << "the stick's own baseline put back\n";
        }
    } else if (ok) {
        std::error_code ec;
        fs::remove(stick.baselineFile(), ec);
        if (ec) {
            ok = false;
            error = "could not remove " + pathToUtf8(stick.baselineFile()) + ": " + ec.message();
        } else {
            std::cout << "the planted baseline removed\n";
        }
    }
    if (!ok) {
        std::cout << error << "\n" << pathToUtf8(stick.rigDir()) << " kept for another try\n";
        return result(false);
    }
    std::error_code ec;
    fs::remove_all(stick.rigDir(), ec);
    return result(!ec);
}

// ---- --plan -------------------------------------------------------------

int plan(const Stick &stick)
{
    application::EngineUpdateStickFacts facts =
        infrastructure::local::readEngineUpdateStickFacts(stick.pioneerUtf8(), stick.engineUtf8());
    std::vector<domain::Track> rekordbox = readRekordbox(stick);
    std::vector<domain::Track> engine;
    {
        rig::EngineReaderAtFileRates reader(stick.engine);
        engine = reader.reader().readTracks();
        std::cout << "sample rates probed: " << reader.probes() << "\n";
    }
    application::completeTracks(rekordbox);
    application::completeTracks(engine);
    const bool hasBaseline = facts.baseline.has_value();
    const std::string root = facts.stickRoot;
    const domain::EngineUpdateProposal proposal = domain::EngineUpdatePlanner::plan(
        application::buildEngineUpdateInput(std::move(rekordbox), std::move(engine), std::move(facts)));

    std::cout << (hasBaseline ? "compared with the record at export " + std::to_string(proposal.baselineSequence)
                              : std::string("no record on this stick"))
              << ", export.pdb at " << proposal.currentSequence << "\n";
    std::size_t checked = 0;
    const auto count = [&checked](const char *name, const auto &rows) {
        std::size_t ticked = 0;
        for (const auto &row : rows) {
            ticked += row.header.checkedByDefault ? 1 : 0;
        }
        checked += ticked;
        std::cout << "  " << name << ": " << rows.size() << " (" << ticked << " ticked)\n";
    };
    count("playlists to create", proposal.playlistsToCreate);
    count("playlists to rename", proposal.playlistsToRename);
    count("playlists to delete", proposal.playlistsToDelete);
    count("tracks to add", proposal.tracksToAdd);
    count("tracks to remove", proposal.tracksToRemove);
    count("membership", proposal.membership);
    count("ratings and comments to Engine", proposal.metadataToEngine);
    count("cues to Engine", proposal.cuesToEngine);
    count("restores onto rekordbox", proposal.restoresToRekordbox);
    count("cues onto rekordbox", proposal.cuesToRekordbox);
    std::cout << "  conflicts: " << proposal.conflicts.size() << "\n"
              << "  Engine's own, kept: " << proposal.engineOwnKept.size() << "\n"
              << "  not added: " << proposal.notAdded.size() << "\n";
    for (const auto &add : proposal.tracksToAdd) {
        std::cout << "  add " << add.stickRelativePath << "  " << add.rekordbox.title << " ("
                  << add.rekordbox.artist << ")\n";
    }
    for (const auto &member : proposal.membership) {
        std::cout << "  " << (member.kind == domain::MembershipEdit::Kind::Add ? "put into \"" : "take out of \"")
                  << member.playlistPath << "\": " << member.pathKey << "\n";
    }
    for (const auto &add : proposal.notAdded) {
        std::cout << "  not added " << add.stickRelativePath << ": " << add.header.reasonText << "\n";
    }
    for (const auto &conflict : proposal.conflicts) {
        std::cout << "  conflict " << conflict.header.key << ": " << conflict.header.reasonText << "\n";
    }
    std::cout << "total: " << checked << " ticked, " << proposal.conflicts.size() << " conflict(s)\n";
    if (proposal.empty()) {
        std::cout << "nothing to do\n";
        return Pass;
    }
    std::cout << "something to write or decide\n";
    return Fail;
}

// ---- --check-back -------------------------------------------------------

int checkBack(const Stick &stick, const std::string &relativeArg)
{
    const std::string key = keyOfRelative(relativeArg);
    const std::vector<domain::Track> rekordbox = readRekordbox(stick);
    const std::vector<domain::Track> engine = readEngineRows(stick);
    const auto rekordboxRows = rowsWithKey(stick, rekordbox, key);
    const auto engineRows = rowsWithKey(stick, engine, key);
    std::cout << "\"" << relativeArg << "\": " << rekordboxRows.size() << " rekordbox row(s), " << engineRows.size()
              << " Engine row(s)\n";
    if (rekordboxRows.size() != 1 || engineRows.size() != 1) {
        return result(false);
    }
    const std::set<std::string> engineLists = engineListPaths(stick);
    std::set<std::string> expected;
    for (const auto &name : playlistNames(*rekordboxRows.front())) {
        if (engineLists.count(name)) {
            expected.insert(name);
        }
    }
    const std::set<std::string> found = playlistNames(*engineRows.front());
    std::set<std::string> missing;
    std::set_difference(expected.begin(), expected.end(), found.begin(), found.end(),
                        std::inserter(missing, missing.end()));
    std::cout << "  Engine row " << engineRows.front()->sourceId << ", \"" << engineRows.front()->title << "\"\n"
              << "  rekordbox's playlists Engine has: " << join(expected) << "\n"
              << "  the Engine row is in: " << join(found) << "\n"
              << "  missing: " << join(missing) << "\n";
    return result(!expected.empty() && missing.empty());
}

// ---- --catalog-shas / --catalog-compare ---------------------------------

int catalogShas(const Stick &stick, const std::string &file)
{
    const auto shas = rig::catalogFileShas(stick.root);
    std::ofstream out(pathFromUtf8(file), std::ios::binary);
    for (const auto &[path, sha] : shas) {
        out << sha << "  " << path << "\n";
        std::cout << "  " << sha << "  " << path << "\n";
    }
    out.close();
    const bool ok = !shas.empty() && out.good()
        && std::none_of(shas.begin(), shas.end(), [](const auto &entry) { return entry.second == "UNREADABLE"; });
    std::cout << shas.size() << " catalog file(s) written down\n";
    return result(ok);
}

int catalogCompare(const Stick &stick, const std::string &file)
{
    std::map<std::string, std::string> recorded;
    {
        std::ifstream in(pathFromUtf8(file), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t gap = line.find("  ");
            if (gap != std::string::npos) {
                recorded[line.substr(gap + 2)] = line.substr(0, gap);
            }
        }
    }
    if (recorded.empty()) {
        std::cout << "nothing recorded in " << file << ", so nothing could be compared\n";
        return result(false);
    }
    const auto now = rig::catalogFileShas(stick.root);
    std::size_t bad = 0;
    for (const auto &[path, sha] : recorded) {
        const auto it = now.find(path);
        if (it == now.end()) {
            std::cout << "  GONE    " << path << "\n";
            ++bad;
        } else if (it->second != sha) {
            std::cout << "  DIFFERS " << path << "\n";
            ++bad;
        } else {
            std::cout << "  same    " << path << "\n";
        }
    }
    for (const auto &[path, sha] : now) {
        if (!recorded.count(path)) {
            std::cout << "  NEW     " << path << "\n";
            ++bad;
        }
    }
    std::cout << recorded.size() << " catalog file(s) compared, " << bad << " not as recorded\n";
    return result(bad == 0);
}

// ---- --sync / --undo ----------------------------------------------------

// Runs the event loop until `done`, or `seconds` pass.
bool waitUntil(const std::function<bool()> &done, int seconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > static_cast<qint64>(seconds) * 1000) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return true;
}

bool analyzeAndWait(RekordboxExportSyncController &controller, const Stick &stick)
{
    controller.analyze(QString::fromStdString(rig::stickLabelFor(stick.root)), QString::fromStdString(stick.pioneerUtf8()),
                       QString::fromStdString(stick.engineUtf8()));
    // A stick of a few thousand tracks reads in well under this; a pass
    // still running then is a failure, not a wait.
    if (!waitUntil([&controller] { return !controller.busy(); }, 1800)) {
        std::cout << "the analysis did not finish in 30 minutes\n";
        return false;
    }
    if (!controller.errorMessage().isEmpty()) {
        std::cout << "the analysis failed: " << controller.errorMessage().toStdString() << "\n";
        return false;
    }
    if (!controller.analyzed()) {
        std::cout << "the analysis did not land\n";
        return false;
    }
    return true;
}

void printAnalysis(RekordboxExportSyncController &controller)
{
    std::cout << "  " << controller.introText().toStdString() << "\n";
    const QVariantMap counts = controller.sectionCounts();
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        std::cout << "  " << it.key().toStdString() << ": " << it.value().toInt() << "\n";
    }
    std::cout << "  ticked: " << controller.checkedCount() << ", conflicts open: " << controller.conflictCount() << "\n";
    for (const auto &row : controller.rows()->rows()) {
        if (row.included) {
            std::cout << "  [x] " << RekordboxExportSyncListModel::sectionName(row.section).toStdString() << " "
                      << row.kind.toStdString() << " " << row.header.key << "\n";
        }
    }
}

gui::LibraryEditSession *sessionOf(const Stick &stick)
{
    auto *registry = gui::EditSessionRegistry::instance();
    const QString id = registry->libraryIdForPath(QString::fromStdString(stick.pioneerUtf8()));
    if (id.isEmpty()) {
        return nullptr;
    }
    return registry->sessionFor(id, QString::fromStdString(rig::stickLabelFor(stick.root)));
}

std::optional<QVariantMap> saveAndWait(gui::LibraryEditSession *session)
{
    std::optional<QVariantMap> summary;
    const auto connection = QObject::connect(session, &gui::LibraryEditSession::saveFinished,
                                             [&summary](const QVariantMap &map) { summary = map; });
    session->save();
    const bool finished = waitUntil([&summary] { return summary.has_value(); }, 1800);
    QObject::disconnect(connection);
    if (!finished) {
        std::cout << "the save did not finish in 30 minutes\n";
    }
    return summary;
}

void printSummary(const QVariantMap &summary)
{
    std::cout << "  summary: " << summary.value("written").toInt() << " of " << summary.value("total").toInt() << " "
              << summary.value("unit").toString().toStdString() << " " << summary.value("verb").toString().toStdString()
              << "; error \"" << summary.value("error").toString().toStdString() << "\", warning \""
              << summary.value("warning").toString().toStdString() << "\"\n";
}

std::string backupDirOf(const Stick &stick)
{
    return infrastructure::backup::backupDirForStickRoot(stick.rootUtf8());
}

std::set<std::string> backupIds(const Stick &stick)
{
    std::set<std::string> ids;
    infrastructure::backup::FilesystemBackupStore store(backupDirOf(stick));
    for (const auto &record : store.list()) {
        ids.insert(record.id);
    }
    return ids;
}

int sync(const Stick &stick)
{
    RekordboxExportSyncController controller;
    std::cout << "analysis:\n";
    if (!analyzeAndWait(controller, stick)) {
        return result(false);
    }
    printAnalysis(controller);
    const int ticked = controller.checkedCount();
    if (ticked == 0) {
        std::cout << "nothing ticked: there is nothing for the save to prove\n";
        return result(false);
    }

    const std::set<std::string> before = backupIds(stick);
    controller.stageSelected();
    if (!controller.errorMessage().isEmpty()) {
        std::cout << "staging failed: " << controller.errorMessage().toStdString() << "\n";
        return result(false);
    }
    gui::LibraryEditSession *session = sessionOf(stick);
    if (session == nullptr) {
        std::cout << "no edit session for this stick\n";
        return result(false);
    }
    std::cout << "staged " << controller.stagedCount() << " row(s) of " << ticked << " ticked, "
              << session->pendingCount() << " change(s):\n";
    for (const auto &line : session->pendingDescriptions()) {
        std::cout << "    " << line.toStdString() << "\n";
    }
    if (controller.stagedCount() != ticked) {
        std::cout << "not every ticked row was staged\n";
        return result(false);
    }

    const std::optional<QVariantMap> summary = saveAndWait(session);
    if (!summary) {
        return result(false);
    }
    printSummary(*summary);
    // The save's records, for --undo: a finished save's undo is kept in
    // this process's session only.
    std::vector<std::string> records;
    for (const auto &id : backupIds(stick)) {
        if (!before.count(id)) {
            records.push_back(id);
        }
    }
    {
        fs::create_directories(stick.rigDir());
        std::ofstream out(stick.rigDir() / "last-save.tsv", std::ios::binary);
        for (const auto &id : records) {
            out << backupDirOf(stick) << "\t" << id << "\n";
        }
    }
    std::cout << "  " << records.size() << " backup record(s) of this save written down for --undo\n";
    const bool saved = summary->value("error").toString().isEmpty() && summary->value("warning").toString().isEmpty()
        && summary->value("written").toInt() == summary->value("total").toInt() && !records.empty();

    // The page analyses again after every save of its session; this waits
    // for that pass, and asks again if none was started.
    std::cout << "analysis after the save:\n";
    if (!waitUntil([&controller] { return !controller.busy(); }, 1800) || !analyzeAndWait(controller, stick)) {
        return result(false);
    }
    printAnalysis(controller);
    std::string baselineError;
    const auto baselineSequence = infrastructure::local::readRekordboxBaselineSequence(stick.root, &baselineError);
    std::cout << "  record on the stick: "
              << (baselineSequence ? "at export " + std::to_string(*baselineSequence) : std::string("none")) << "\n";
    return result(saved && controller.checkedCount() == 0 && controller.hasBaseline());
}

int undo(const Stick &stick)
{
    std::vector<gui::UndoableBackup> backups;
    {
        std::ifstream in(stick.rigDir() / "last-save.tsv", std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t tab = line.find('\t');
            if (tab != std::string::npos) {
                backups.push_back({QString::fromStdString(line.substr(0, tab)), QString::fromStdString(line.substr(tab + 1))});
            }
        }
    }
    if (backups.empty()) {
        std::cout << "no save of --sync written down on this stick, so nothing to undo\n";
        return result(false);
    }
    RekordboxExportSyncController controller;
    std::cout << "analysis before the undo:\n";
    if (!analyzeAndWait(controller, stick)) {
        return result(false);
    }
    printAnalysis(controller);
    gui::LibraryEditSession *session = sessionOf(stick);
    if (session == nullptr) {
        std::cout << "no edit session for this stick\n";
        return result(false);
    }
    // What LibraryEditSession::undoLastSave stages, over the records the
    // save left.
    if (!session->stage(std::make_unique<gui::RestoreBackupsChange>(backups))) {
        std::cout << "the undo could not be staged\n";
        return result(false);
    }
    std::cout << "undoing " << backups.size() << " backup record(s)\n";
    const std::optional<QVariantMap> summary = saveAndWait(session);
    if (!summary) {
        return result(false);
    }
    printSummary(*summary);
    const bool undone = summary->value("error").toString().isEmpty();
    std::cout << "what came back:\n";
    if (!waitUntil([&controller] { return !controller.busy(); }, 1800) || !analyzeAndWait(controller, stick)) {
        return result(false);
    }
    printAnalysis(controller);
    if (undone) {
        std::error_code ec;
        fs::remove(stick.rigDir() / "last-save.tsv", ec);
    }
    return result(undone);
}

int usage()
{
    std::cerr << "usage: rig_engine_update --rekordbox <PIONEER> --engine <Engine Library> <subcommand>\n"
                 "  --pick | --plan | --sync | --undo | --restore-plant\n"
                 "  --plant-baseline-without <stick-relative path> | --check-back <stick-relative path>\n"
                 "  --catalog-shas <file> | --catalog-compare <file>\n";
    return Error;
}

}  // namespace

int main(int argc, char **argv)
{
    std::optional<std::string> rekordboxArg;
    std::optional<std::string> engineArg;
    std::string command;
    std::string argument;
    const std::set<std::string> withArgument = {"--plant-baseline-without", "--check-back", "--catalog-shas",
                                                "--catalog-compare"};
    const std::set<std::string> bare = {"--pick", "--plan", "--sync", "--undo", "--restore-plant"};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--rekordbox" || arg == "--engine") && i + 1 < argc) {
            (arg == "--rekordbox" ? rekordboxArg : engineArg) = argv[++i];
        } else if (withArgument.count(arg) && i + 1 < argc && command.empty()) {
            command = arg;
            argument = argv[++i];
        } else if (bare.count(arg) && command.empty()) {
            command = arg;
        } else {
            return usage();
        }
    }
    if (!rekordboxArg || !engineArg || command.empty()) {
        return usage();
    }

    Stick stick;
    stick.pioneer = catalogDir(*rekordboxArg);
    stick.engine = catalogDir(*engineArg);
    stick.root = pathFromUtf8(infrastructure::paths::stickRootForCatalogPath(pathToUtf8(stick.engine)));
    if (pathFromUtf8(infrastructure::paths::stickRootForCatalogPath(pathToUtf8(stick.pioneer))) != stick.root) {
        std::cout << "the rekordbox and Engine libraries are not on one stick\nRIG RESULT: FAIL\n";
        return Error;
    }
    if (rig::isProtectedStick(stick.root) || rig::isProtectedStick(stick.pioneer) || rig::isProtectedStick(stick.engine)) {
        std::cout << "REFUSED: " << pathToUtf8(stick.root)
                  << " is WHALESHARK, WHALESHARK2 or CORSAIR, which no rig tool reads or writes. Nothing was done.\n"
                  << "RIG RESULT: FAIL\n";
        return Error;
    }
    if (!fs::is_regular_file(stick.pioneer / "rekordbox" / "export.pdb") || !fs::is_regular_file(stick.mdb())) {
        std::cout << "needs both " << pathToUtf8(stick.pioneer / "rekordbox" / "export.pdb") << " and "
                  << pathToUtf8(stick.mdb()) << "\nRIG RESULT: FAIL\n";
        return Error;
    }

    QCoreApplication app(argc, argv);
    try {
        if (command == "--pick") {
            return pick(stick);
        }
        if (command == "--plant-baseline-without") {
            return plant(stick, argument);
        }
        if (command == "--restore-plant") {
            return restorePlant(stick);
        }
        if (command == "--plan") {
            return plan(stick);
        }
        if (command == "--check-back") {
            return checkBack(stick, argument);
        }
        if (command == "--catalog-shas") {
            return catalogShas(stick, argument);
        }
        if (command == "--catalog-compare") {
            return catalogCompare(stick, argument);
        }
        if (command == "--sync") {
            return sync(stick);
        }
        if (command == "--undo") {
            return undo(stick);
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return Error;
    }
    return usage();
}
