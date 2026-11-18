// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A stick file being read must not stand in anyone's way: while a backup
// reads it, a writer can still delete or rename it, and what the backup
// reads is what the file held when it was opened. On Windows a plain
// std::ifstream refused both, and a SQLite writer committing while a
// backup read its rollback journal failed with "disk I/O error"
// (backup_database_capture_test case 4, 6 of 10 runs).

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "infrastructure/paths/shared_read_stream.hpp"
#include "infrastructure/stick_backup/file_entry_source.hpp"
#include "scratch_path.hpp"

using seabass::infrastructure::SharedReadStream;
using seabass::infrastructure::stick_backup::FileEntrySource;
namespace fs = std::filesystem;

namespace
{

void writeFile(const fs::path &p, const std::string &content)
{
    std::ofstream out(p, std::ios::binary);
    out << content;
}

std::string readAll(FileEntrySource &source)
{
    std::string all;
    std::vector<std::byte> buffer(7);  // small, so the reads go on after the delete
    for (;;) {
        const std::size_t got = source.read(buffer);
        all.append(reinterpret_cast<const char *>(buffer.data()), got);
        if (got < buffer.size()) {
            return all;
        }
    }
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_shared_read_stream_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string content = "a rollback journal, deleted by the commit that wrote it";

    // Deleted while open: the delete goes through, and the reader still
    // gets every byte, with nothing taken for a device refusing.
    {
        const fs::path journal = root / "m.db-journal";
        writeFile(journal, content);
        FileEntrySource source(journal);
        assert(source.ok());
        std::error_code ec;
        fs::remove(journal, ec);
        assert(!ec && "a file being read must still be deletable");
        assert(readAll(source) == content);
        assert(!source.readFailed() && "a file deleted while read has ended, it did not fail");
        std::cout << "case 1 (deleted while read: the delete works, every byte is read, no failure) OK\n";
    }

    // Renamed away while open: the rename goes through and the reader
    // keeps the file it opened. (A rename ONTO a file that is open is
    // still refused on Windows, share delete or not: its default rename
    // does not replace an open target.)
    {
        const fs::path file = root / "m.db";
        const fs::path aside = root / "m.db.old";
        writeFile(file, content);
        SharedReadStream in(file);
        assert(in);
        std::error_code ec;
        fs::rename(file, aside, ec);
        assert(!ec && "a file being read must still be renamable");
        std::string first(content.size(), '\0');
        in.read(first.data(), static_cast<std::streamsize>(first.size()));
        assert(first == content && "and the reader keeps the file it opened");
        std::cout << "case 2 (renamed away while read: the rename works, the reader keeps its file) OK\n";
    }

    // The size is the open file's.
    {
        const fs::path file = root / "sized.db";
        writeFile(file, content);
        SharedReadStream in(file);
        assert(in.sizeNow() && *in.sizeNow() == content.size());
        std::cout << "case 3 (the open file says how long it is) OK\n";
    }

    // Opening something that is not there fails as an ifstream does.
    {
        SharedReadStream in(root / "not-there.db");
        assert(!in);
        assert(!in.sizeNow());
        std::cout << "case 4 (a missing file does not open) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
