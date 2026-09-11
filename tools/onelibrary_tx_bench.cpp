// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Part C of the save-cost investigation: what one OneLibrary cue write
// actually costs on a real stick, split into the two things it does per
// item -- opening the encrypted database (SQLCipher derives the key from a
// passphrase on every open) and committing one transaction.
//
// OneLibraryCueWriter opens the database twice per call today, once to
// write and once to verify, so the "today" shape below opens twice.
//
// SAFETY: works on a COPY of exportLibrary.db placed under
// <stick>/.seabass-writebench-tx/, which it removes afterwards. The real
// database is only read.
//
// Build:
//   g++ -std=c++23 -O2 -I src -I third_party/kaitai_struct_cpp_stl_runtime \
//       tools/onelibrary_tx_bench.cpp -o build/onelibrary_tx_bench \
//       build/libseabass_core.a build/librekordbox_format.a \
//       build/libkaitai_cpp_stl_runtime.a -lz -ldl -lpthread

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

namespace fs = std::filesystem;
namespace ol = seabass::infrastructure::onelibrary;
using Clock = std::chrono::steady_clock;

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: onelibrary_tx_bench <stick-mount-point> [items]\n";
        return 1;
    }
    const fs::path stick = argv[1];
    const int items = argc > 2 ? std::atoi(argv[2]) : 50;
    const std::string pioneerRoot = (stick / "PIONEER").string();
    if (!ol::OneLibraryCueWriter::existsFor(pioneerRoot)) {
        std::cerr << "no exportLibrary.db under " << pioneerRoot << "\n";
        return 1;
    }

    const fs::path scratch = stick / ".seabass-writebench-tx";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const fs::path copy = scratch / "exportLibrary.db";
    fs::copy_file(ol::OneLibraryCueWriter::dbPathFor(pioneerRoot), copy);
    std::cout << "Working on a copy: " << copy << " (" << (fs::file_size(copy) / 1024) << " KB)\n";

    const std::string key = ol::deriveOneLibraryKey();
    auto seconds = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    auto row = [&](const std::string &label, double s) {
        std::cout << "  " << std::left << std::setw(52) << label << std::right << std::setw(8) << std::fixed
                  << std::setprecision(2) << s << " s" << std::setw(9) << std::setprecision(1)
                  << (s * 1000.0 / items) << " ms/item\n";
    };

    // Today: a fresh writer per cue means two opens (write + verify) and one
    // transaction, every single time.
    auto t0 = Clock::now();
    for (int i = 0; i < items; ++i) {
        {
            ol::SqlCipherLibrary lib;
            ol::SqlCipherDb db(lib, copy.string(), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec("BEGIN IMMEDIATE;");
            db.exec("UPDATE content SET path = path WHERE rowid = " + std::to_string(1 + i) + ";");
            db.exec("COMMIT;");
        }
        {
            ol::SqlCipherLibrary verifyLib;
            ol::SqlCipherDb verifyDb(verifyLib, copy.string(), /*readOnly=*/true);
            verifyDb.exec("PRAGMA key = '" + key + "';");
            verifyDb.exec("SELECT count(*) FROM content WHERE rowid = " + std::to_string(1 + i) + ";");
        }
    }
    double todayShape = seconds(t0);

    // Proposed: one connection held for the whole save, one transaction per
    // cue, one verification pass at the end.
    t0 = Clock::now();
    {
        ol::SqlCipherLibrary lib;
        ol::SqlCipherDb db(lib, copy.string(), /*readOnly=*/false);
        db.exec("PRAGMA key = '" + key + "';");
        for (int i = 0; i < items; ++i) {
            db.exec("BEGIN IMMEDIATE;");
            db.exec("UPDATE content SET path = path WHERE rowid = " + std::to_string(1 + i) + ";");
            db.exec("COMMIT;");
        }
    }
    {
        ol::SqlCipherLibrary verifyLib;
        ol::SqlCipherDb verifyDb(verifyLib, copy.string(), /*readOnly=*/true);
        verifyDb.exec("PRAGMA key = '" + key + "';");
        verifyDb.exec("SELECT count(*) FROM content;");
    }
    double heldShape = seconds(t0);

    std::cout << "\n== OneLibrary cue write, " << items << " items ==\n";
    row("today: two opens + one transaction, per item", todayShape);
    row("proposed: one open for the save, one tx per item", heldShape);

    fs::remove_all(scratch);
    std::cout << "\nScratch removed. The real exportLibrary.db was only read.\n";
    return 0;
}
