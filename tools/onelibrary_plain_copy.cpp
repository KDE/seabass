// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Standalone investigation tool: writes an unencrypted copy of an
// exportLibrary.db, so it can be queried with the plain sqlite3 shell or
// Python instead of a purpose-built C++ tool per question.
//
// Written for issue #8, to find out what the OneLibrary rows that
// export.pdb lacks have in common -- a question that needs every column of
// `content` and a few joins, asked several different ways.
//
// Not part of the CMake build, like tools/onelibrary_audit.cpp:
//
//   g++ -std=c++20 -I<repo>/src tools/onelibrary_plain_copy.cpp \
//       src/infrastructure/onelibrary/sqlcipher_dyn.cpp \
//       src/infrastructure/onelibrary/onelibrary_key.cpp \
//       src/infrastructure/work_counters.cpp \
//       -ldl -lz -o onelibrary_plain_copy
//   ./onelibrary_plain_copy <copy of exportLibrary.db> <plain.db>
//
// Opens the source read-WRITE: SQLite opens an attached database with the
// main connection's flags, so a read-only source leaves nothing to export
// into. Run it on a copy, never on a mounted stick's own file. The output
// must not exist yet.
#include <filesystem>
#include <iostream>
#include <string>

#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"

using namespace seabass::infrastructure::onelibrary;

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: onelibrary_plain_copy <exportLibrary.db> <plain.db>\n";
        return 1;
    }
    if (std::filesystem::exists(seabass::pathFromUtf8(argv[2]))) {
        std::cerr << argv[2] << " exists; refusing to write into it\n";
        return 1;
    }
    // A quote in the path would end the SQL string literal.
    if (std::string(argv[2]).find('\'') != std::string::npos) {
        std::cerr << "the output path must not contain a single quote\n";
        return 1;
    }
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, argv[1], false);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    db.exec(std::string("ATTACH DATABASE '") + argv[2] + "' AS plain KEY '';");
    db.exec("SELECT sqlcipher_export('plain');");
    db.exec("DETACH DATABASE plain;");
    std::cout << "wrote " << argv[2] << "\n";
    return 0;
}
