// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Issue #59 on hardware: where a OneLibrary player (the OMNIS-DUO first)
// takes its cues from. See onelibrary_cue_source_probe_lib.hpp for the five
// plants and what each one answers.
//
//   onelibrary_cue_source_probe candidates <stick root>
//       Lists the tracks prepare would choose from, and the five it would
//       pick. Reads only.
//
//   onelibrary_cue_source_probe prepare <stick root> --backup <archive.zip> --snapshot <out.json>
//       Plants T1..T5, reads every plant back and fails loudly if one is
//       not exact, writes the snapshot of the prepared state to <out.json>
//       (and the card beside it, <out>.card.txt), and prints the CARD.
//       Refuses a protected stick (WHALESHARK*, CORSAIR*), a stick whose
//       backup archive is not named or not there (take it first, with
//       rig_backup, which verifies it), and running rekordbox or Engine DJ.
//       exportLibrary.db's write-ahead log is folded first if it holds
//       anything, as Seabass folds it after a save; a hot rollback journal
//       is rolled back by the OneLibrary reader, as the app does (#48).
//
//   onelibrary_cue_source_probe readback <stick root> <snapshot.json>
//       After the deck session: what changed against the snapshot -- the
//       five tracks' cue lists (PCO2 and PCOB, both files) and cue table
//       rows, their content rows (cueUpdateCount included), every
//       exportLibrary.db table, the history tables row by row, and every
//       file on the stick added, removed or changed (another track's
//       analysis file decoded). Reads only: the database is read from a
//       copy on this computer.
//
// The last line is "RESULT: PASS" or "RESULT: FAIL" for prepare and
// candidates; readback ends with the number of differences, exit 0.

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/sqlite_pending_journal.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"
#include "gui/qt_path.hpp"
#include "onelibrary_cue_source_probe_lib.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

int usage()
{
    std::cerr << "usage: onelibrary_cue_source_probe candidates <stick root>\n"
                 "       onelibrary_cue_source_probe prepare <stick root> --backup <archive.zip> --snapshot <out.json>\n"
                 "       onelibrary_cue_source_probe readback <stick root> <snapshot.json>\n";
    return 2;
}

std::string upper(std::string text)
{
    for (char &c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

// The same list as rig-platform.sh's is_protected_label and rig_format:
// Sebastian's working sticks and the reference are never written.
bool isProtected(const fs::path &root)
{
    for (const auto &part : root) {
        const std::string name = upper(pathToUtf8(part));
        if (name.rfind("WHALESHARK", 0) == 0 || name.rfind("CORSAIR", 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string label(const fs::path &root)
{
    const std::string name = pathToUtf8(root.lexically_normal().filename());
    return name.empty() ? pathToUtf8(root) : name;
}

void describeLog(const std::string &pioneer)
{
    const fs::path db = pathFromUtf8(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer));
    for (const char *suffix : {"-wal", "-shm", "-journal"}) {
        fs::path side = db;
        side += suffix;
        std::error_code ec;
        std::cout << "  exportLibrary.db" << suffix << ": "
                  << (fs::exists(side, ec) ? std::to_string(fs::file_size(side, ec)) + " bytes" : std::string("absent"))
                  << "\n";
    }
    std::cout << "  hot rollback journal: " << (infrastructure::hasPendingJournal(db) ? "YES" : "no") << "\n";
}

int candidates(const std::string &root)
{
    auto all = omnis59::findCandidates(root, true, std::cout);
    for (const auto &c : all) {
        std::cout << "  " << c.title << " / " << c.artist << "  (" << static_cast<int>(c.durationSeconds) << " s, "
                  << c.contentPath << ")\n";
    }
    const auto five = omnis59::pickFive(all);
    std::cout << "would pick:\n";
    for (size_t i = 0; i < five.size(); ++i) {
        std::cout << "  T" << i + 1 << " " << five[i].title << " / " << five[i].artist << "\n";
    }
    std::cout << "RESULT: " << (five.size() == 5 ? "PASS" : "FAIL") << "\n";
    return five.size() == 5 ? 0 : 1;
}

int prepare(const std::string &root, const std::string &backup, const std::string &snapshotPath)
{
    const fs::path rootPath = pathFromUtf8(root);
    if (isProtected(rootPath)) {
        std::cout << root << " is a protected stick; nothing is written to it\nRESULT: FAIL\n";
        return 1;
    }
    std::error_code ec;
    if (!fs::is_regular_file(pathFromUtf8(backup), ec) || fs::file_size(pathFromUtf8(backup), ec) == 0) {
        std::cout << "no backup archive at " << backup << "; take one first (rig_backup <stick> <archive.zip>)\n"
                  << "RESULT: FAIL\n";
        return 1;
    }
    if (const std::string blocker = infrastructure::system::conflictingDjSoftwareName(); !blocker.empty()) {
        std::cout << blocker << " is running; close it first\nRESULT: FAIL\n";
        return 1;
    }
    const std::string pioneer = pathToUtf8(rootPath / "PIONEER");
    if (!infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
        std::cout << "no OneLibrary (exportLibrary.db) on " << root << "\nRESULT: FAIL\n";
        return 1;
    }
    std::cout << "stick " << label(rootPath) << " at " << root << ", backup " << backup << "\n"
              << "exportLibrary.db before writing:\n";
    describeLog(pioneer);
    {
        fs::path wal = pathFromUtf8(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer));
        wal += "-wal";
        if (fs::exists(wal, ec) && fs::file_size(wal, ec) > 0) {
            const auto left = infrastructure::onelibrary::OneLibraryCueWriter::foldLogOf(
                infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer));
            std::cout << "  folded the write-ahead log: " << (left ? std::to_string(*left) : std::string("?"))
                      << " bytes left\n";
            if (!left || *left != 0) {
                std::cout << "RESULT: FAIL\n";
                return 1;
            }
        }
    }

    try {
        const auto five = omnis59::pickFive(omnis59::findCandidates(root, true, std::cout));
        if (five.size() != 5) {
            std::cout << "fewer than five usable tracks\nRESULT: FAIL\n";
            return 1;
        }
        for (size_t i = 0; i < five.size(); ++i) {
            std::cout << "  T" << i + 1 << " " << five[i].title << " / " << five[i].artist << "  content_id "
                      << five[i].contentId << ", export.pdb id " << five[i].deviceLibraryId << ", "
                      << five[i].analysisFile << "\n";
        }
        std::cout << "planting:\n";
        omnis59::plant(root, five, std::cout);

        std::cout << "reading back:\n";
        const auto problems = omnis59::verifyPlants(root, five);
        for (const auto &p : problems) {
            std::cout << "  NOT AS PLANTED: " << p << "\n";
        }
        if (!problems.empty()) {
            std::cout << "the stick is not as planned; restore it from " << backup
                      << " (rig_restore) before taking it to the deck\nRESULT: FAIL\n";
            return 1;
        }
        std::cout << "  all five exactly as planted\nexportLibrary.db after writing:\n";
        describeLog(pioneer);

        QJsonObject snap = omnis59::snapshot(root, five);
        const std::string cardText = omnis59::card(five, label(rootPath));
        snap["backup"] = QString::fromStdString(backup);
        snap["card"] = QString::fromStdString(cardText);
        QFile out(gui::pathToQString(pathFromUtf8(snapshotPath)));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || out.write(QJsonDocument(snap).toJson(QJsonDocument::Indented)) <= 0) {
            std::cout << "could not write " << snapshotPath << "\nRESULT: FAIL\n";
            return 1;
        }
        out.close();
        std::ofstream cardFile(pathFromUtf8(snapshotPath + ".card.txt"));
        cardFile << cardText;
#ifndef _WIN32
        ::sync();
#endif
        std::cout << "snapshot: " << snapshotPath << " (" << snap["files"].toObject().size() << " files hashed)\n\n"
                  << cardText << "\nRESULT: PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cout << "FAILED: " << e.what() << "\nrestore the stick from " << backup
                  << " (rig_restore) before taking it to the deck\nRESULT: FAIL\n";
        return 1;
    }
}

int readbackMode(const std::string &root, const std::string &snapshotPath)
{
    QFile in(gui::pathToQString(pathFromUtf8(snapshotPath)));
    if (!in.open(QIODevice::ReadOnly)) {
        std::cerr << "cannot read " << snapshotPath << "\n";
        return 2;
    }
    const QJsonObject snap = QJsonDocument::fromJson(in.readAll()).object();
    if (snap["tool"].toString() != "onelibrary_cue_source_probe") {
        std::cerr << snapshotPath << " is not a snapshot of this tool\n";
        return 2;
    }
    try {
        omnis59::readback(root, snap, std::cout);
    } catch (const std::exception &e) {
        std::cerr << "readback failed: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        return usage();
    }
    const std::string mode = argv[1];
    const std::string root = argv[2];
    if (mode == "candidates" && argc == 3) {
        return candidates(root);
    }
    if (mode == "readback" && argc == 4) {
        return readbackMode(root, argv[3]);
    }
    if (mode == "prepare" && argc == 7) {
        std::string backup;
        std::string snapshot;
        for (int i = 3; i + 1 < argc; i += 2) {
            const std::string flag = argv[i];
            if (flag == "--backup") {
                backup = argv[i + 1];
            } else if (flag == "--snapshot") {
                snapshot = argv[i + 1];
            }
        }
        if (backup.empty() || snapshot.empty()) {
            return usage();
        }
        return prepare(root, backup, snapshot);
    }
    return usage();
}
