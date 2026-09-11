// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Standalone debug tool: dumps a real exportLibrary.db's `content` table
// schema and, for every column, how many rows actually have a non-null/
// non-empty/non-zero value. Useful whenever a real-hardware audit is
// needed of which OneLibrary/Device Library Plus fields are genuinely
// populated in practice (vs. merely present in the schema) -- e.g. to
// decide whether domain::Track should start capturing a field the
// format supports but real libraries never fill in.
//
// Not part of the CMake build (this is a one-off investigation tool, not
// a test or shipped binary) -- build it directly against the sources it
// needs:
//
//   g++ -std=c++20 -I<repo>/src tools/onelibrary_audit.cpp \
//       src/infrastructure/onelibrary/sqlcipher_dyn.cpp \
//       src/infrastructure/onelibrary/onelibrary_key.cpp \
//       -ldl -lz -o onelibrary_audit
//
// Then run against a *copy* of a real exportLibrary.db (read-only open,
// but copy first anyway -- don't point this at a mounted stick's
// original file):
//
//   ./onelibrary_audit /path/to/exportLibrary.db
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

using namespace seabass::infrastructure::onelibrary;

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: onelibrary_audit <path-to-exportLibrary.db>\n";
        return 1;
    }
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, argv[1], true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");

    std::vector<std::string> colNames;
    {
        SqlCipherStatement cols(db, "PRAGMA table_info(content)");
        while (cols.step()) {
            colNames.push_back(cols.columnText(1));
        }
    }

    int64_t totalRows = 0;
    {
        SqlCipherStatement cnt(db, "SELECT COUNT(*) FROM content");
        cnt.step();
        totalRows = cnt.columnInt64(0);
    }
    std::cout << totalRows << " rows in content\n\n";
    std::cout << "column: non-null/non-empty/non-zero row count\n";

    for (const auto &col : colNames) {
        SqlCipherStatement st(db, "SELECT COUNT(*) FROM content WHERE " + col + " IS NOT NULL AND " + col +
                                       " != '' AND " + col + " != 0");
        st.step();
        std::cout << col << ": " << st.columnInt64(0) << " / " << totalRows << "\n";
    }

    return 0;
}
