// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: give Library Health's OneLibrary cue-table check (#57)
// something to repair -- rig check W11.
//
//   rig_plant_cue_drift <stick root> --plant
//   rig_plant_cue_drift <stick root> --restore
//
// The drift #57 repairs is a OneLibrary row whose cue table holds cues
// its analysis file does not: what Sync Cue Points left before afa3dbdb.
// The rig's references carry none (every row's table is empty), so this
// makes some.
//
// --plant copies PIONEER/rekordbox/exportLibrary.db, with its -wal and
// -shm where they are there, into <stick root>/RIG-HIDDEN-CUES/ and
// records which of the three it found in RIG-HIDDEN-CUES/planted.tsv.
// It then writes a pad A loop at 0:17.778 to 0:21.333 (the drift seen on
// TESTSHARK) and a pad B hot cue at 0:08.000 into the cue table of three
// OneLibrary rows whose analysis file was read and holds neither of those
// cues, whose cue table is empty and whose file no other row lists, so
// both cues are ones the table holds and the file does not. Rows whose
// file holds no cue at all go first; the rig's references have one, as
// rekordbox marks a cue of its own on nearly every track.
//
// Only the cue table is written. OneLibraryCueWriter writes the analysis
// file first and the table after, so it is handed a stand-in stick root
// with no PIONEER folder in it: it finds no analysis file there and
// writes the table alone. The analysis files of the three tracks are
// hashed before and after, and a change fails the plant. export.pdb and
// Engine are not opened for writing.
//
// Writing folds exportLibrary.db's write-ahead log, which takes its -wal
// and -shm away; where the log was empty, both are put back as they were.
//
// --restore puts the three files back as they were (a sidecar that was
// not there is removed) and removes RIG-HIDDEN-CUES. Nothing planted is
// a PASS.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "domain/cue_tolerance.hpp"
#include "domain/onelibrary_cue_table.hpp"
#include "domain/track.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
namespace ol = seabass::infrastructure::onelibrary;

namespace
{

constexpr std::size_t kTracksToPlant = 3;
const char *const kDbFiles[] = {"exportLibrary.db", "exportLibrary.db-wal", "exportLibrary.db-shm"};

fs::path hiddenDir(const fs::path &root)
{
    return root / "RIG-HIDDEN-CUES";
}

fs::path rekordboxDir(const fs::path &root)
{
    return root / "PIONEER" / "rekordbox";
}

std::string fileSha(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return infrastructure::hashing::toHex(infrastructure::hashing::Sha256::of(bytes));
}

int restore(const fs::path &root)
{
    const fs::path hidden = hiddenDir(root);
    const fs::path record = hidden / "planted.tsv";
    if (!fs::exists(record)) {
        std::cout << "nothing planted on this stick\nRIG RESULT: PASS\n";
        return 0;
    }
    // Every file is checked before any is put back: a half restore is
    // worse than none.
    std::map<std::string, bool> wasThere;
    std::ifstream in(record);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            wasThere[line.substr(0, tab)] = line.substr(tab + 1) == "present";
        }
    }
    in.close();
    int bad = 0;
    for (const char *name : kDbFiles) {
        if (!wasThere.contains(name)) {
            std::cout << "CANNOT RESTORE: the record does not say whether " << name << " was there\n";
            ++bad;
        } else if (wasThere[name] && !fs::is_regular_file(hidden / name)) {
            std::cout << "CANNOT RESTORE: the copy of " << name << " is missing from " << pathToUtf8(hidden) << "\n";
            ++bad;
        }
    }
    if (bad != 0) {
        std::cout << "RIG RESULT: FAIL\n";
        return 1;
    }
    for (const char *name : kDbFiles) {
        const fs::path target = rekordboxDir(root) / name;
        if (wasThere[name]) {
            fs::copy_file(hidden / name, target, fs::copy_options::overwrite_existing);
            std::cout << "put back " << pathToUtf8(target) << "\n";
        } else if (fs::exists(target)) {
            fs::remove(target);
            std::cout << "removed " << pathToUtf8(target) << ", which was not there before\n";
        }
    }
    fs::remove_all(hidden);
    std::cout << "RIG RESULT: PASS\n";
    return 0;
}

int plant(const fs::path &root)
{
    const fs::path hidden = hiddenDir(root);
    const fs::path record = hidden / "planted.tsv";
    if (fs::exists(record)) {
        std::cout << "already planted; run --restore first\nRIG RESULT: FAIL\n";
        return 1;
    }
    const std::string pioneer = pathToUtf8(root / "PIONEER");
    if (!ol::OneLibraryCueWriter::existsFor(pioneer)) {
        std::cout << "no OneLibrary on this stick (" << ol::OneLibraryCueWriter::dbPathFor(pioneer)
                  << "), so no cue table to plant in\nRIG RESULT: FAIL\n";
        return 1;
    }

    domain::CuePoint loop;
    loop.kind = domain::CuePoint::Kind::Hot;
    loop.hotCueNumber = 1;
    loop.positionMs = 17778;
    loop.isLoop = true;
    loop.loopEndMs = 21333;
    domain::CuePoint hot;
    hot.kind = domain::CuePoint::Kind::Hot;
    hot.hotCueNumber = 2;
    hot.positionMs = 8000;
    const std::vector<domain::CuePoint> cues = {loop, hot};
    // The table as the plant leaves it, to ask the check's own rule
    // whether the file already holds any of it.
    std::vector<domain::CueTableEntry> planted;
    for (const domain::CuePoint &cue : cues) {
        planted.push_back({static_cast<int64_t>(planted.size() + 1), cue.hotCueNumber, cue});
    }

    std::cout << "reading OneLibrary:\n";
    const std::vector<domain::Track> rows = ol::OneLibraryReader(pioneer).readAll();
    const ol::OneLibraryCueTables tables(pioneer);
    std::map<std::string, int> rowsPerFile;
    for (const domain::Track &row : rows) {
        ++rowsPerFile[row.filePath];
    }
    // The loop ends at 0:21.333, so a track shorter than that cannot hold it.
    std::vector<const domain::Track *> eligible;
    for (const domain::Track &row : rows) {
        if (!row.filePath.empty() && rowsPerFile[row.filePath] == 1 && domain::analysisFileRead(row)
            && tables.of(std::stoll(row.sourceId)).empty() && row.durationSeconds >= 30
            && domain::entriesNotInFile(planted, row.cues, domain::cueToleranceMsFor(row.bpm, row.bpm)).size()
                   == planted.size()) {
            eligible.push_back(&row);
        }
    }
    std::cout << "  " << rows.size() << " rows, " << eligible.size()
              << " with an analysis file read that holds none of the planted cues, an empty cue table and a file of"
                 " their own\n";
    if (eligible.empty()) {
        std::cout << "nothing to plant on\nRIG RESULT: FAIL\n";
        return 1;
    }
    std::sort(eligible.begin(), eligible.end(), [](const domain::Track *a, const domain::Track *b) {
        if (a->cues.empty() != b->cues.empty()) {
            return a->cues.empty();
        }
        return std::stoll(a->sourceId) < std::stoll(b->sourceId);
    });
    eligible.resize(std::min(eligible.size(), kTracksToPlant));

    // The pristine library first, and the record last, so a record always
    // has its copies beside it.
    fs::create_directories(hidden);
    std::string recordText;
    for (const char *name : kDbFiles) {
        const fs::path file = rekordboxDir(root) / name;
        const bool there = fs::is_regular_file(file);
        if (there) {
            fs::copy_file(file, hidden / name, fs::copy_options::overwrite_existing);
        }
        recordText += std::string(name) + '\t' + (there ? "present" : "absent") + '\n';
    }
    std::ofstream(record) << recordText;

    // Where the analysis files are, to prove afterwards they were not written.
    const fs::path scratch = hidden / "stand-in-root";
    fs::create_directories(scratch);
    std::map<std::string, std::string> analysisBefore;
    try {
        ol::OneLibraryCueWriter onStick(pioneer);
        for (const domain::Track *row : eligible) {
            for (const std::string &file : onStick.cueFilesForPath(row->filePath)) {
                analysisBefore[file] = fileSha(pathFromUtf8(file));
            }
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\n";
        restore(root);
        std::cout << "RIG RESULT: FAIL\n";
        return 1;
    }

    bool ok = true;
    try {
        // The database is the stick's; file paths are looked up against
        // the stand-in root, which holds no PIONEER folder, so the writer
        // finds no analysis file and writes the cue table alone.
        ol::OneLibraryCueWriter writer(pioneer, pathToUtf8(scratch));
        for (const domain::Track *row : eligible) {
            const fs::path relative = fs::relative(pathFromUtf8(row->filePath), root);
            writer.writeCuesForPath(pathToUtf8(scratch / relative), cues);
            std::cout << "planted on content_id " << row->sourceId << " \"" << row->artist << " - " << row->title
                      << "\" (" << row->filePath << ", analysis file " << row->analysisFile
                      << "): " << domain::describeCuePlaces(cues) << "; the analysis file holds "
                      << (row->cues.empty() ? std::string("no cue") : domain::describeCuePlaces(row->cues)) << "\n";
        }
        writer.finishWriting();
        // Folding the log closed the last connection, and SQLite deleted
        // the -wal and -shm with it. A rekordbox export leaves them beside
        // the database (the rig's references have an empty -wal), and W11
        // compares the -wal byte for byte before and after its save and
        // undo, so where the log was empty both go back as they were: the
        // plant then differs from the stick in the cue table alone. A -shm
        // beside an empty log holds nothing SQLite keeps; it rebuilds it.
        const fs::path wal = hidden / "exportLibrary.db-wal";
        if (fs::is_regular_file(wal) && fs::file_size(wal) == 0) {
            for (const char *name : {"exportLibrary.db-wal", "exportLibrary.db-shm"}) {
                if (fs::is_regular_file(hidden / name) && !fs::exists(rekordboxDir(root) / name)) {
                    fs::copy_file(hidden / name, rekordboxDir(root) / name);
                }
            }
        } else if (fs::is_regular_file(wal)) {
            std::cout << "the write-ahead log held " << fs::file_size(wal)
                      << " bytes, which are now folded into exportLibrary.db\n";
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\n";
        ok = false;
    }
    fs::remove_all(scratch);
    for (const auto &[file, sha] : analysisBefore) {
        if (fileSha(pathFromUtf8(file)) != sha) {
            std::cout << "the analysis file " << file << " changed, which the plant must never do\n";
            ok = false;
        }
    }
    if (!ok) {
        std::cout << "putting the library back:\n";
        restore(root);
        std::cout << "RIG RESULT: FAIL\n";
        return 1;
    }
    std::cout << "planted " << eligible.size() << " row(s); " << analysisBefore.size()
              << " analysis file(s) checked unchanged\nRIG RESULT: PASS\n";
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    const std::string mode = argc == 3 ? argv[2] : "";
    if (mode != "--plant" && mode != "--restore") {
        std::cerr << "usage: rig_plant_cue_drift <stick root> --plant|--restore\n";
        return 2;
    }
    try {
        const fs::path root = pathFromUtf8(argv[1]);
        return mode == "--plant" ? plant(root) : restore(root);
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }
}
