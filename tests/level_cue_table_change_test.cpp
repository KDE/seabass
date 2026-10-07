// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Library Health's #57 repair through the real save loop, on a copy of the
// anonymized fixture (its rekordbox directory copied as PIONEER). The
// fixture came off a stick a dev build had synced: OneLibrary row 392's cue
// table holds pads B and E its analysis file does not, beside A, C, D and
// a memory cue it does.
//
// Planted on the copy: a comment on 392's pad A, hot cue bank links for
// its pads A and B, a table cue on a row whose analysis file is not on the
// stick, and a cue of kind 12 (no kind Seabass knows) on another row. The
// check must list 392 alone and count the two planted rows apart. The
// repair, staged for all three rows, must remove B and E and B's bank
// link and nothing else: A, C, D and the memory cue keep their rows as
// they were (cue_id, colour, comment, A's bank link), the planted rows are
// untouched, no analysis file and no export.pdb is written, and the
// database is backed up first. A second save finds nothing left to do.
//
//   level_cue_table_change_test <tests/fixtures/anonymized_library>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
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

struct Db
{
    ol::SqlCipherLibrary lib;
    ol::SqlCipherDb db;
    Db(const std::string &pioneer, bool readOnly) : db(lib, ol::OneLibraryCueWriter::dbPathFor(pioneer), readOnly)
    {
        db.exec("PRAGMA key = '" + ol::deriveOneLibraryKey() + "';");
    }
};

// Every cue row as (cue_id, content_id, kind, inUsec, outUsec,
// colorTableIndex, cueComment), in cue_id order.
using CueRow = std::tuple<int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, std::string>;
std::vector<CueRow> cueRows(const std::string &pioneer)
{
    Db d(pioneer, /*readOnly=*/true);
    ol::SqlCipherStatement select(d.db, "SELECT cue_id, content_id, kind, inUsec, outUsec, colorTableIndex, "
                                        "ifnull(cueComment, '') FROM cue ORDER BY cue_id");
    std::vector<CueRow> rows;
    while (select.step()) {
        rows.emplace_back(select.columnInt64(0), select.columnInt64(1), select.columnInt64(2), select.columnInt64(3),
                          select.columnInt64(4), select.columnInt64(5), select.columnText(6));
    }
    return rows;
}

std::set<int64_t> bankLinkedCues(const std::string &pioneer)
{
    Db d(pioneer, /*readOnly=*/true);
    ol::SqlCipherStatement select(d.db, "SELECT cue_id FROM hotCueBankList_cue");
    std::set<int64_t> ids;
    while (select.step()) {
        ids.insert(select.columnInt64(0));
    }
    return ids;
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

std::string fileBytes(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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

    // 1. The fixture as it came: content_id 392 alone, holding B and E
    // beyond its file.
    const auto asCopied = audit(pioneer);
    check(asCopied.excess.size() == 1 && asCopied.excess[0].row.sourceId == "392",
          "on the fixture, exactly content_id 392 holds cues its analysis file does not");
    check(asCopied.noReadableFile == 0 && asCopied.notUnderstood == 0, "the fixture has no row to leave alone");
    if (asCopied.excess.size() != 1) {
        return 1;
    }
    const domain::Track row392 = asCopied.excess[0].row;
    const auto excess392 = asCopied.excess[0].notInFile;
    check(domain::describeCuePlaces(domain::cuesOf(excess392)) == "pad B at 1:07.751, pad E at 2:15.251",
          "392's extra cues are pads B and E, got " + domain::describeCuePlaces(domain::cuesOf(excess392)));
    int64_t padA = 0;
    int64_t padB = 0;
    for (const auto &e : asCopied.excess[0].table) {
        padA = e.kind == 1 ? e.cueId : padA;
        padB = e.kind == 2 ? e.cueId : padB;
    }
    check(padA != 0 && padB != 0, "392's table has pads A and B");

    // Two rows to leave alone, from rows with an empty table and a file
    // that was read.
    std::vector<domain::Track> spare;
    {
        const ol::OneLibraryCueTables tables(pioneer);
        for (const auto &t : ol::OneLibraryReader(pioneer).readAll()) {
            if (spare.size() < 2 && tables.of(std::stoll(t.sourceId)).empty() && domain::analysisFileRead(t)
                && !t.cues.empty()) {
                spare.push_back(t);
            }
        }
    }
    check(spare.size() == 2, "the fixture has two rows to plant on");
    if (spare.size() != 2) {
        return 1;
    }
    const int64_t noFileRow = std::stoll(spare[0].sourceId);
    const int64_t oddKindRow = std::stoll(spare[1].sourceId);
    {
        Db d(pioneer, /*readOnly=*/false);
        d.db.exec("UPDATE cue SET cueComment = 'kept as it was' WHERE cue_id = " + std::to_string(padA) + ";");
        d.db.exec("INSERT INTO hotCueBankList_cue (hotCueBankList_id, cue_id, sequenceNo) VALUES (1, "
                  + std::to_string(padA) + ", 1), (1, " + std::to_string(padB) + ", 2);");
        d.db.exec("UPDATE content SET analysisDataFilePath = '/PIONEER/USBANLZ/P000/00000000/ANLZ0000.DAT' "
                  "WHERE content_id = " + std::to_string(noFileRow) + ";");
        d.db.exec("INSERT INTO cue (content_id, kind, colorTableIndex, isActiveLoop, inUsec, outUsec) VALUES ("
                  + std::to_string(noFileRow) + ", 3, 0, 0, 30000000, 30000000), (" + std::to_string(oddKindRow)
                  + ", 12, 0, 0, 1000000, 1000000);");
    }

    // 2. With the plants: still 392 alone listed, the two counted apart.
    const auto before = audit(pioneer);
    check(before.excess.size() == 1 && before.excess[0].row.sourceId == "392", "the planted rows are not listed");
    check(before.noReadableFile == 1, "the row with no analysis file is counted apart, got "
                                          + std::to_string(before.noReadableFile));
    check(before.notUnderstood == 1, "the row with kind 12 is counted apart, got " + std::to_string(before.notUnderstood));
    check(before.rowsRead
              == before.emptyTable + before.withinFile + before.noReadableFile + before.notUnderstood
                     + static_cast<int>(before.excess.size()),
          "every row falls in one count");
    const auto rowsBefore = cueRows(pioneer);
    const auto usbanlzBefore = filesUnder(pioneerDir / "USBANLZ");
    const std::string pdbBefore = fileBytes(pioneerDir / "rekordbox" / "export.pdb");
    std::cout << "case 1 (the check lists 392 alone and counts the planted rows apart) done\n";

    // 3. Repair All, with the planted rows staged as well: the save checks
    // each row again.
    const QString pioneerQ = pathToQString(pioneerDir);
    application::CancellationToken token;
    const auto save = [&]() {
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, pioneerQ, QString());
        std::vector<LevelCueTableChange::Row> rows = {
            {392, row392.filePath, row392.title, row392.bpm},
            {noFileRow, spare[0].filePath, spare[0].title, spare[0].bpm},
            {oddKindRow, spare[1].filePath, spare[1].title, spare[1].bpm}};
        auto change = std::make_shared<LevelCueTableChange>(pioneerQ, rows);
        const SaveLoopResult result = runSaveLoop({change}, ctx);
        const bool backedUp = !ctx.backupIdOf(ol::OneLibraryCueWriter::dbPathFor(pioneer)).empty();
        return std::make_tuple(result, backedUp, change->unitsWritten());
    };
    {
        const auto [result, backedUp, repaired] = save();
        check(result.error.isEmpty(), "the save succeeds: " + result.error.toStdString());
        check(result.appliedIds.size() == 1 && result.skippedIds.isEmpty(), "the repair is applied, not skipped");
        check(backedUp, "exportLibrary.db is backed up before the write");
        check(repaired == 1, "one track repaired, got " + std::to_string(repaired));
    }
    std::cout << "case 2 (the save) done\n";

    // 4. B and E gone with B's bank link; every other cue row exactly as
    // it was; no file but the database written.
    const auto after = audit(pioneer);
    check(after.excess.empty(), "after the repair no row holds cues its analysis file does not");
    check(after.noReadableFile == 1 && after.notUnderstood == 1, "the rows left alone are still there to count");
    std::set<int64_t> removedIds;
    for (const auto &e : excess392) {
        removedIds.insert(e.cueId);
    }
    std::vector<CueRow> expected;
    for (const auto &r : rowsBefore) {
        if (!removedIds.count(std::get<0>(r))) {
            expected.push_back(r);
        }
    }
    const auto rowsAfter = cueRows(pioneer);
    check(rowsAfter.size() == rowsBefore.size() - 2, "exactly two cue rows removed");
    check(rowsAfter == expected, "every other cue row is exactly as it was: cue_id, colour, comment");
    bool commentKept = false;
    for (const auto &r : rowsAfter) {
        commentKept = commentKept || (std::get<0>(r) == padA && std::get<6>(r) == "kept as it was");
    }
    check(commentKept, "pad A keeps its comment");
    const auto links = bankLinkedCues(pioneer);
    check(!links.count(padB), "pad B's bank link went with it");
    check(links.count(padA) == 1, "pad A's bank link stays");
    check(filesUnder(pioneerDir / "USBANLZ") == usbanlzBefore, "every analysis file is byte-identical");
    check(fileBytes(pioneerDir / "rekordbox" / "export.pdb") == pdbBefore, "export.pdb is byte-identical");
    std::cout << "case 3 (B and E removed, nothing else written) done\n";

    // 5. A second save finds nothing left to do.
    {
        const auto [result, backedUp, repaired] = save();
        (void)backedUp;
        check(result.error.isEmpty() && result.skippedIds.size() == 1 && repaired == 0,
              "a second repair finds nothing to do");
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
