// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Library Health's #57 repair through the real save loop, on a copy of the
// anonymized fixture (its rekordbox directory copied as PIONEER). The
// fixture came off a stick a dev build had synced: OneLibrary row 392's cue
// table holds pads B and E its analysis file does not. LevelCueTableChange
// must set that table to the file's cues, keeping the colours of the cues
// both hold, back the database up first, and write nothing else: no
// analysis file, no export.pdb, no other row's cues. A second save finds
// nothing left to do.
//
//   level_cue_table_change_test <tests/fixtures/anonymized_library>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/onelibrary_cue_table.hpp"
#include "gui/edit/changes/level_cue_table_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;
namespace ol = seabass::infrastructure::onelibrary;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

domain::CueTableAudit audit(const std::string &pioneer)
{
    const auto tracks = ol::OneLibraryReader(pioneer).readAll();
    const ol::OneLibraryCueTables tables(pioneer);
    return domain::auditCueTables(tracks, [&tables](const domain::Track &t) { return tables.of(std::stoll(t.sourceId)); });
}

// Every cue row as (content_id, kind, inUsec, outUsec, colorTableIndex),
// in cue_id order.
using CueRow = std::tuple<int64_t, int64_t, int64_t, int64_t, int64_t>;
std::vector<CueRow> cueRows(const std::string &pioneer)
{
    ol::SqlCipherLibrary lib;
    ol::SqlCipherDb db(lib, ol::OneLibraryCueWriter::dbPathFor(pioneer), /*readOnly=*/true);
    db.exec("PRAGMA key = '" + ol::deriveOneLibraryKey() + "';");
    ol::SqlCipherStatement select(db, "SELECT content_id, kind, inUsec, outUsec, colorTableIndex FROM cue ORDER BY cue_id");
    std::vector<CueRow> rows;
    while (select.step()) {
        rows.emplace_back(select.columnInt64(0), select.columnInt64(1), select.columnInt64(2), select.columnInt64(3),
                          select.columnInt64(4));
    }
    return rows;
}

// Every file under `dir`, by path, with its bytes.
std::map<std::string, std::string> filesUnder(const fs::path &dir)
{
    std::map<std::string, std::string> files;
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            std::ifstream in(entry.path(), std::ios::binary);
            files[pathToUtf8(entry.path())] = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    return files;
}

bool samePlaces(const std::vector<domain::CuePoint> &a, const std::vector<domain::CuePoint> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].hotCueNumber != b[i].hotCueNumber || a[i].isLoop != b[i].isLoop
            || std::abs(a[i].positionMs - b[i].positionMs) > 0.001) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: level_cue_table_change_test <anonymized_library fixture dir>\n";
        return 2;
    }
    const fs::path stick = testing::scratchRoot() / "seabass_level_cue_table_change_test";
    fs::remove_all(stick);
    const fs::path pioneerDir = stick / "PIONEER";
    fs::create_directories(stick);
    // A copy: opening the fixture's database in place would drop its -shm/-wal.
    fs::copy(pathFromUtf8(argv[1]) / "rekordbox", pioneerDir, fs::copy_options::recursive);
    const std::string pioneer = pathToUtf8(pioneerDir);

    // 1. The fixture's one damaged row, and only it.
    const auto before = audit(pioneer);
    check(before.excess.size() == 1 && before.excess[0].row.sourceId == "392",
          "before the repair, exactly content_id 392 holds cues its analysis file does not");
    if (before.excess.empty()) {
        return 1;
    }
    const domain::Track row392 = before.excess[0].row;
    const auto rowsBefore = cueRows(pioneer);
    const auto usbanlzBefore = filesUnder(pioneerDir / "USBANLZ");
    const auto pdbBefore = filesUnder(pioneerDir / "rekordbox");
    std::map<std::pair<int64_t, int64_t>, int64_t> colourBefore;  // row 392's, by (kind, inUsec)
    size_t untouchedWithin = 0;  // rows with a table within their file, outside 392
    for (const auto &[contentId, kind, in, out, colour] : rowsBefore) {
        if (contentId == 392) {
            colourBefore[{kind, in}] = colour;
        } else {
            ++untouchedWithin;
        }
    }
    check(untouchedWithin > 0, "the fixture has cue table rows outside content_id 392 to watch");
    std::cout << "case 1 (the check finds content_id 392 alone) done\n";

    // 2. The repair, through the save loop.
    const QString pioneerQ = pathToQString(pioneerDir);
    application::CancellationToken token;
    const auto save = [&]() {
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, pioneerQ, QString());
        const SaveLoopResult result = runSaveLoop(
            {std::make_shared<LevelCueTableChange>(
                pioneerQ, LevelCueTableChange::Row{392, row392.filePath, row392.title, row392.bpm})},
            ctx);
        const bool backedUp = !ctx.backupIdOf(ol::OneLibraryCueWriter::dbPathFor(pioneer)).empty();
        return std::make_pair(result, backedUp);
    };
    {
        const auto [result, backedUp] = save();
        check(result.error.isEmpty(), "the save succeeds: " + result.error.toStdString());
        check(result.appliedIds.size() == 1 && result.skippedIds.isEmpty(), "the repair is applied, not skipped");
        check(backedUp, "exportLibrary.db is backed up before the write");
    }
    std::cout << "case 2 (the save) done\n";

    // 3. Row 392's table is its file's cues, colours kept where kind and
    // position match; nothing else changed.
    const auto after = audit(pioneer);
    check(after.excess.empty(), "after the repair no row holds cues its analysis file does not");
    check(after.rowsRead == before.rowsRead, "the same rows are read after the repair");
    {
        const ol::OneLibraryCueTables tables(pioneer);
        check(samePlaces(tables.of(392), row392.cues), "content_id 392's table holds exactly its analysis file's cues");
    }
    const auto rowsAfter = cueRows(pioneer);
    std::vector<CueRow> othersBefore;
    std::vector<CueRow> othersAfter;
    size_t kept = 0;
    for (const auto &r : rowsBefore) {
        if (std::get<0>(r) != 392) {
            othersBefore.push_back(r);
        }
    }
    for (const auto &r : rowsAfter) {
        if (std::get<0>(r) != 392) {
            othersAfter.push_back(r);
            continue;
        }
        const auto was = colourBefore.find({std::get<1>(r), std::get<2>(r)});
        if (was != colourBefore.end()) {
            check(std::get<4>(r) == was->second, "a cue the table and file both held keeps its colour");
            ++kept;
        }
    }
    check(kept == 4, "the four cues table and file both held (A, C, D, memory) are kept, found " + std::to_string(kept));
    check(othersAfter == othersBefore, "no other row's cue table changed");
    check(filesUnder(pioneerDir / "USBANLZ") == usbanlzBefore, "every analysis file is byte-identical");
    const auto pdbAfter = filesUnder(pioneerDir / "rekordbox");
    check(pdbAfter.count(pathToUtf8(pioneerDir / "rekordbox" / "export.pdb"))
              && pdbAfter.at(pathToUtf8(pioneerDir / "rekordbox" / "export.pdb"))
                     == pdbBefore.at(pathToUtf8(pioneerDir / "rekordbox" / "export.pdb")),
          "export.pdb is byte-identical");
    std::cout << "case 3 (row 392 levelled, nothing else written) done\n";

    // 4. A second save of the same repair finds the table within the file
    // and leaves it alone.
    {
        const auto [result, backedUp] = save();
        (void)backedUp;
        check(result.error.isEmpty() && result.skippedIds.size() == 1, "a second repair of the same row is skipped");
    }
    check(cueRows(pioneer) == rowsAfter, "the skipped repair wrote nothing");
    std::cout << "case 4 (nothing left to repair) done\n";

    fs::remove_all(stick);
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "level_cue_table_change_test: OK\n";
    return 0;
}
