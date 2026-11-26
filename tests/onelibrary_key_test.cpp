// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// oneLibraryKeyPragma() keys a connection with a key derived here rather
// than with the passphrase. rekordbox and the players have the passphrase
// alone, so whatever such a connection writes must still open with it:
// that is what this test opens every database with in the end.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;
using seabass::infrastructure::onelibrary::deriveOneLibraryKey;
using seabass::infrastructure::onelibrary::oneLibraryKeyPragma;
using seabass::infrastructure::onelibrary::SqlCipherDb;
using seabass::infrastructure::onelibrary::SqlCipherLibrary;
using seabass::infrastructure::onelibrary::SqlCipherStatement;

namespace
{

std::string passphrasePragma()
{
    return "PRAGMA key = '" + deriveOneLibraryKey() + "';";
}

bool usesTheDerivedKey(const std::string &pragma)
{
    return pragma.rfind("PRAGMA key = \"x'", 0) == 0;
}

// The fixture's database alone, its write-ahead log folded in, so that the
// file is the whole database.
fs::path freshDatabase(const std::string &name)
{
    const fs::path scratch = seabass::testing::scratchRoot() / pathFromUtf8(name);
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const fs::path source = pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox"
        / "rekordbox";
    for (const char *file : {"exportLibrary.db", "exportLibrary.db-wal"}) {
        fs::copy_file(source / file, scratch / file);
    }
    const fs::path db = scratch / "exportLibrary.db";
    {
        SqlCipherLibrary lib;
        SqlCipherDb opened(lib, pathToUtf8(db), /*readOnly=*/false);
        opened.exec(passphrasePragma());
        opened.exec("PRAGMA wal_checkpoint(TRUNCATE);");
    }
    fs::remove(scratch / "exportLibrary.db-wal", ec);
    fs::remove(scratch / "exportLibrary.db-shm", ec);
    return db;
}

std::string firstBytes(const fs::path &file, std::size_t count)
{
    std::string bytes(count, '\0');
    std::ifstream in(file, std::ios::binary);
    in.read(bytes.data(), static_cast<std::streamsize>(count));
    assert(in.gcount() == static_cast<std::streamsize>(count));
    return bytes;
}

int64_t count(const fs::path &db, const std::string &keyPragma, const std::string &sql)
{
    SqlCipherLibrary lib;
    SqlCipherDb opened(lib, pathToUtf8(db), /*readOnly=*/true);
    opened.exec(keyPragma);
    SqlCipherStatement statement(opened, sql);
    const bool row = statement.step();
    assert(row);
    return statement.columnInt64(0);
}

}  // namespace

int main()
{
    // 1. A real database is keyed with the derived key, and reads the same
    //    with it as with the passphrase.
    {
        const fs::path db = freshDatabase("onelibrary_key_test-1");
        const std::string pragma = oneLibraryKeyPragma(pathToUtf8(db));
        assert(usesTheDerivedKey(pragma) && "a database with a salt gets the key derived from it");
        assert(pragma == oneLibraryKeyPragma(pathToUtf8(db)) && "and the same one each time");
        const int64_t tracks = count(db, passphrasePragma(), "SELECT count(*) FROM content");
        assert(tracks == 1644);
        assert(count(db, pragma, "SELECT count(*) FROM content") == tracks);
        std::cout << "case 1 (the derived key reads what the passphrase reads) OK\n";
    }

    // 2. What a connection keyed that way writes, the passphrase opens.
    {
        const fs::path db = freshDatabase("onelibrary_key_test-2");
        const std::string pragma = oneLibraryKeyPragma(pathToUtf8(db));
        assert(usesTheDerivedKey(pragma));
        {
            SqlCipherLibrary lib;
            SqlCipherDb opened(lib, pathToUtf8(db), /*readOnly=*/false);
            opened.exec(pragma);
            opened.exec("CREATE TABLE seabass_key_test(n integer);");
            opened.exec("INSERT INTO seabass_key_test(n) VALUES (1), (2), (3);");
            opened.exec("PRAGMA wal_checkpoint(TRUNCATE);");
        }
        assert(count(db, passphrasePragma(), "SELECT count(*) FROM seabass_key_test") == 3);
        assert(count(db, passphrasePragma(), "SELECT count(*) FROM content") == 1644);
        std::cout << "case 2 (written with the derived key, opened with the passphrase) OK\n";
    }

    // 3. The file emptied, or gone, after its salt was read and before the
    //    connection reads it: the connection starts a database of its own
    //    there. It is one the passphrase opens, with the salt the key was
    //    made for, not one keyed for a salt it no longer has.
    for (const bool removed : {false, true}) {
        const fs::path db = freshDatabase(removed ? "onelibrary_key_test-3b" : "onelibrary_key_test-3a");
        const std::string pragma = oneLibraryKeyPragma(pathToUtf8(db));
        assert(usesTheDerivedKey(pragma));
        const std::string salt = firstBytes(db, 16);
        if (removed) {
            fs::remove(db);
        } else {
            std::ofstream(db, std::ios::binary | std::ios::trunc);
            assert(fs::file_size(db) == 0);
        }
        {
            SqlCipherLibrary lib;
            SqlCipherDb opened(lib, pathToUtf8(db), /*readOnly=*/false);
            opened.exec(pragma);
            opened.exec("CREATE TABLE seabass_key_test(n integer);");
            opened.exec("INSERT INTO seabass_key_test(n) VALUES (7);");
        }
        assert(fs::file_size(db) > 0);
        assert(firstBytes(db, 16) == salt && "the database started there has the salt the key was made for");
        assert(count(db, passphrasePragma(), "SELECT count(*) FROM seabass_key_test") == 1);
        std::cout << "case 3 (" << (removed ? "the file gone" : "the file emptied")
                  << " before the open: still the passphrase's database) OK\n";
    }

    // 4. Nothing to read a salt from: the passphrase.
    {
        const fs::path scratch = seabass::testing::scratchRoot() / "onelibrary_key_test-4";
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch);
        assert(oneLibraryKeyPragma(pathToUtf8(scratch / "missing.db")) == passphrasePragma());
        {
            std::ofstream shortFile(scratch / "short.db", std::ios::binary);
            shortFile << "fifteen bytes..";
        }
        assert(fs::file_size(scratch / "short.db") == 15);
        assert(oneLibraryKeyPragma(pathToUtf8(scratch / "short.db")) == passphrasePragma());
        {
            std::ofstream plain(scratch / "plain.db", std::ios::binary);
            plain.write("SQLite format 3\0and the rest of a header", 40);
        }
        assert(oneLibraryKeyPragma(pathToUtf8(scratch / "plain.db")) == passphrasePragma());
        std::cout << "case 4 (missing, short and plain files get the passphrase) OK\n";
    }

    std::cout << "onelibrary_key_test: all cases passed\n";
    return 0;
}
