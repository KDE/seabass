// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A cue the rekordbox and OneLibrary writers cannot store (before the start
// of the track, at a time that is not a number, past what the analysis
// files hold) is left out by both, and the save log says so: one line per
// track, naming it by its stick-relative path, and the other cues are
// written.
//
// Driven through runSaveLoop, as a page's save is, over a copy of the
// fixture's rekordbox catalog. One save of four Add Cues:
//
//   - track A: a cue at 1234 ms (fits), then one at -500 ms (does not),
//     then one at 2345 ms. Each Add Cue writes the whole set into
//     DeviceLibrary and into Device Library Plus, so the -500 ms cue is
//     left out by two writers: one line for A, not two. The third Add Cue
//     reads A again and finds no -500 ms cue to leave out.
//   - track B: a loop whose end is past what the files hold.
//
// Afterwards both catalogs hold A's two new cues and every cue A and B
// had, and neither holds the cues left out.

#include <QString>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using namespace seabass::domain;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "ok: " : "FAILED: ") << what << "\n";
    if (!ok) {
        ++failures;
    }
}

fs::path freshCopy(const std::string &name)
{
    const fs::path scratch = seabass::testing::scratchRoot() / seabass::pathFromUtf8(name);
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const fs::path source =
        seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
    const fs::path pioneerRoot = scratch / "PIONEER";
    fs::copy(source, pioneerRoot, fs::copy_options::recursive);
    return pioneerRoot;
}

std::vector<Track> readRekordbox(const fs::path &pioneerRoot)
{
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(seabass::pathToUtf8(pioneerRoot));
    return reader.readAll();
}

std::vector<Track> readOneLibrary(const fs::path &pioneerRoot)
{
    seabass::infrastructure::onelibrary::OneLibraryReader reader(seabass::pathToUtf8(pioneerRoot));
    return reader.readAll();
}

std::optional<Track> find(const std::vector<Track> &tracks, const std::string &filePath)
{
    for (const Track &track : tracks) {
        if (track.filePath == filePath) {
            return track;
        }
    }
    return std::nullopt;
}

// The hot cues, by slot: position.
std::map<int, double> hotCues(const std::vector<CuePoint> &cues)
{
    std::map<int, double> bySlot;
    for (const CuePoint &cue : cues) {
        if (cue.kind == CuePoint::Kind::Hot) {
            bySlot[cue.hotCueNumber] = cue.positionMs;
        }
    }
    return bySlot;
}

bool sameHotCues(const std::map<int, double> &a, const std::map<int, double> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (const auto &[slot, position] : a) {
        const auto found = b.find(slot);
        if (found == b.end() || std::abs(found->second - position) > 1.0) {
            return false;
        }
    }
    return true;
}

// The hot cues in a content row's cue table.
std::map<int, double> cueTableOf(const fs::path &pioneerRoot, const std::string &contentId)
{
    using namespace seabass::infrastructure::onelibrary;
    const std::string dbPath = OneLibraryCueWriter::dbPathFor(seabass::pathToUtf8(pioneerRoot));
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, dbPath, /*readOnly=*/true);
    db.exec(oneLibraryKeyPragma(dbPath));
    std::vector<CuePoint> cues;
    for (const CueTableEntry &entry : readCueTable(db, std::stoll(contentId))) {
        if (entry.understood()) {
            cues.push_back(entry.cue);
        }
    }
    return hotCues(cues);
}

struct Target
{
    Track track;
    std::vector<int> freeSlots;
};

// rekordbox tracks with a hot cue (so the analysis file exists), a file
// path, a row in Device Library Plus and three free hot cue slots.
std::vector<Target> findTargets(const fs::path &pioneerRoot)
{
    const std::vector<Track> oneLibrary = readOneLibrary(pioneerRoot);
    std::vector<Target> targets;
    for (const Track &track : readRekordbox(pioneerRoot)) {
        if (track.filePath.empty() || hotCues(track.cues).empty() || !find(oneLibrary, track.filePath)) {
            continue;
        }
        Target target{track, {}};
        const auto used = hotCues(track.cues);
        for (int slot = 1; slot <= 8 && target.freeSlots.size() < 3; ++slot) {
            if (!used.count(slot)) {
                target.freeSlots.push_back(slot);
            }
        }
        if (target.freeSlots.size() == 3) {
            targets.push_back(target);
        }
        if (targets.size() == 2) {
            break;
        }
    }
    return targets;
}

std::shared_ptr<PendingChange> addCue(const fs::path &pioneerRoot, const Target &target, int slot, double positionMs,
                                      bool isLoop = false, double loopEndMs = 0.0)
{
    const QString root = seabass::gui::pathToQString(pioneerRoot);
    return std::make_shared<AddCueChange>(QStringLiteral("rekordbox"), root, QString::fromStdString(target.track.sourceId),
                                          positionMs, QStringLiteral("hot"), slot, QStringLiteral("#FF0000"), QString(),
                                          isLoop, loopEndMs, QString::fromStdString(target.track.title));
}

std::string fileBytes(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// The log's lines that contain `needle`.
std::vector<std::string> linesWith(const std::string &log, const std::string &needle)
{
    std::vector<std::string> found;
    std::istringstream lines(log);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find(needle) != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

}  // namespace

// Each cue counted once, by its position first, then a loop's end.
void countedByWhy()
{
    const auto cue = [](double position, bool isLoop = false, double loopEnd = 0.0) {
        CuePoint c;
        c.kind = CuePoint::Kind::Memory;
        c.positionMs = position;
        c.isLoop = isLoop;
        c.loopEndMs = loopEnd;
        return c;
    };
    const double nan = std::nan("");
    const std::vector<CuePoint> cues = {cue(0.0),          cue(1000.0, true, 2000.0), cue(-1.0),
                                        cue(nan),          cue(5.0e9),                cue(1000.0, true, nan),
                                        cue(-5.0, true, nan), cue(1000.0, true, 5.0e9), cue(4294967295.0)};
    const auto counted = seabass::infrastructure::rekordbox::cuesWithNoPlace(cues);
    check(counted.beforeStart == 2 && counted.notATime == 2 && counted.pastTheField == 2 && counted.total() == 6,
          "counted by why: 2 before the start, 2 not a time, 2 past the field");
    check(seabass::infrastructure::rekordbox::writableCues(cues).size() == cues.size() - counted.total(),
          "and exactly those are left out by the writers");
}

int main()
{
    countedByWhy();
    const fs::path pioneerRoot = freshCopy("seabass_cues_left_out_log");
    const fs::path stickRoot = pioneerRoot.parent_path();
    const std::vector<Target> targets = findTargets(pioneerRoot);
    if (targets.size() != 2) {
        std::cerr << "FAILED: the fixture must have two rekordbox tracks with a hot cue, a Device Library Plus row "
                     "and three free slots\n";
        return 1;
    }
    const Target &a = targets[0];
    const Target &b = targets[1];
    const std::string aPath = seabass::application::stickRelativePathOf(a.track.filePath, seabass::pathToUtf8(stickRoot));
    const std::string bPath = seabass::application::stickRelativePathOf(b.track.filePath, seabass::pathToUtf8(stickRoot));
    check(!aPath.empty() && !bPath.empty(), "both tracks are on the stick: " + aPath + ", " + bPath);

    std::vector<std::shared_ptr<PendingChange>> changes = {
        addCue(pioneerRoot, a, a.freeSlots[0], 1234.0),
        addCue(pioneerRoot, a, a.freeSlots[1], -500.0),
        addCue(pioneerRoot, a, a.freeSlots[2], 2345.0),
        addCue(pioneerRoot, b, b.freeSlots[0], 1000.0, /*isLoop=*/true, 5.0e9),
    };
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    SaveLoopResult result;
    {
        SaveContext ctx(token, noProgress, {}, seabass::gui::pathToQString(pioneerRoot), {});
        result = runSaveLoop(changes, ctx);
    }
    check(result.error.isEmpty(), "the save goes through: " + result.error.toStdString());
    check(result.appliedIds.size() == 4, "every change is applied");

    const fs::path logPath = stickRoot / "Seabass" / "seabass.log";
    const std::string log = fs::exists(logPath) ? fileBytes(logPath) : std::string();
    const std::string said = "have no place on the track and were left out";
    const std::vector<std::string> all = linesWith(log, said);
    const std::vector<std::string> forA = linesWith(log, " of " + aPath + " " + said);
    const std::vector<std::string> forB = linesWith(log, " of " + bPath + " " + said);
    for (const std::string &line : all) {
        std::cout << "    " << line << "\n";
    }
    check(linesWith(log, "add-cue: also wrote into OneLibrary").size() == 4,
          "each Add Cue wrote DeviceLibrary and Device Library Plus, both writers of the cues left out");
    check(all.size() == 2, "two lines in the save log for cues left out, one per track: " + std::to_string(all.size()));
    check(forA.size() == 1 && forA.front().find("1 cue point(s) of " + aPath) != std::string::npos
              && forA.front().find("1 before the start of the track") != std::string::npos,
          "track A's line, once, names it with how many and why");
    check(forB.size() == 1 && forB.front().find("1 cue point(s) of " + bPath) != std::string::npos
              && forB.front().find("1 later than the analysis files can hold") != std::string::npos,
          "track B's line, once, names it with how many and why");

    // The other cues are written, in both catalogs.
    std::map<int, double> expectA = hotCues(a.track.cues);
    expectA[a.freeSlots[0]] = 1234.0;
    expectA[a.freeSlots[2]] = 2345.0;
    const std::map<int, double> expectB = hotCues(b.track.cues);
    const std::vector<Track> rekordbox = readRekordbox(pioneerRoot);
    const std::vector<Track> oneLibrary = readOneLibrary(pioneerRoot);
    const auto rekordboxA = find(rekordbox, a.track.filePath);
    const auto rekordboxB = find(rekordbox, b.track.filePath);
    const auto oneLibraryA = find(oneLibrary, a.track.filePath);
    const auto oneLibraryB = find(oneLibrary, b.track.filePath);
    check(rekordboxA && sameHotCues(hotCues(rekordboxA->cues), expectA),
          "DeviceLibrary: track A has its cues and the two new ones, not the one left out");
    check(rekordboxB && sameHotCues(hotCues(rekordboxB->cues), expectB),
          "DeviceLibrary: track B has its cues, not the ones left out");
    check(oneLibraryA && sameHotCues(cueTableOf(pioneerRoot, oneLibraryA->sourceId), expectA),
          "Device Library Plus's cue table: track A the same");
    check(oneLibraryB && sameHotCues(cueTableOf(pioneerRoot, oneLibraryB->sourceId), expectB),
          "Device Library Plus's cue table: track B the same");

    if (failures > 0) {
        std::cout << "cues_left_out_log_test: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "cues_left_out_log_test: all passed\n";
    return 0;
}
