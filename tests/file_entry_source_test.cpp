// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// FileEntrySource::readFailed(): a read the device refused, told apart
// from a file that ended, on every standard library. See the class
// comment for why the stream's own flags cannot be the judge.

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/stick_backup/file_entry_source.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::stick_backup::FileEntrySource;

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_file_entry_source_test";
    fs::remove_all(root);
    fs::create_directories(root);
    std::vector<std::byte> buffer(1 << 16);

    // A file read to its end has ended. Not a failure, however many times
    // the caller asks past it.
    {
        const fs::path file = root / "whole.bin";
        std::ofstream(file, std::ios::binary) << std::string(10'000, 'a');
        FileEntrySource source(file);
        assert(source.ok());
        assert(source.read(buffer) == 10'000);
        assert(source.read(buffer) == 0);
        assert(!source.readFailed() && "an end of file is not a refusal");
        std::cout << "case 1 (a clean end of file is not a failure) OK\n";
    }

    // A file something shrank while it was read ended too: on a healthy
    // stick that is a writer, which the database capture retries rather
    // than reports as a fault.
    {
        const fs::path file = root / "shrinks.bin";
        std::ofstream(file, std::ios::binary) << std::string(100'000, 'b');
        FileEntrySource source(file);
        std::vector<std::byte> first(4096);
        assert(source.read(first) == 4096);
        fs::resize_file(file, 4096);
        std::size_t more = 0;
        while (const std::size_t got = source.read(buffer)) {
            more += got;
        }
        assert(!source.readFailed() && "a file that is no longer than what was read has ended");
        std::cout << "case 2 (a file shrunk mid-read has ended, not failed) OK\n";
    }

    // read(2) failing: EISDIR on a directory, the one refusal every
    // platform can produce without damaged media. libstdc++ reports it
    // with badbit, libc++ with failbit and eofbit -- the flags of a clean
    // end of file -- so this is what catches a guard that trusts them.
    // Where the directory does not open at all (Windows), there is
    // nothing to read and ok() says so.
    {
        const fs::path dir = root / "a-directory";
        fs::create_directories(dir);
        FileEntrySource source(dir);
        if (source.ok()) {
            assert(source.read(buffer) == 0);
            assert(source.readFailed() && "a read the system refused is a failure on every standard library");
        }
        std::cout << "case 3 (a refused read is a failure whatever the stream's flags say) OK\n";
    }

    fs::remove_all(root);
    std::cout << "file_entry_source_test: all cases passed\n";
    return 0;
}
