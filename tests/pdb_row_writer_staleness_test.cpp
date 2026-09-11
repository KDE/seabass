// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "infrastructure/rekordbox/pdb_row_writer.hpp"

#include "scratch_path.hpp"

// PdbRowWriter's staleness guard, against the one case that twice defeated
// weaker versions of it: an external rewrite of the .pdb, in the SAME
// process, at IDENTICAL length.
//
// Why that case specifically, and why a dedicated test rather than another
// case inside pdb_row_writer_test.cpp: the guard has now shipped broken
// twice, and both times the whole existing suite stayed green.
//
//   - A size/mtime-only guard is blind here by construction. The length is
//     unchanged so file_size() can't move, and on Windows an in-process
//     path-based fs::last_write_time() genuinely does not observe a write
//     that just happened (confirmed empirically on Windows 11/NTFS: the
//     directory entry is refreshed on handle close/reopen, so a separate
//     *process* sees the new timestamp while the writing process does
//     not). Both halves of the cheap pre-filter miss it at once.
//   - A `PRAGMA data_version` guard (tried on the OneLibrary side, which
//     adapted this class's convention) is blind for an unrelated reason:
//     data_version is a per-connection counter that only moves when *that
//     same connection* observes another connection's commit, so a
//     guard that opens a fresh connection per check reads the same value
//     forever.
//
// What survives both is comparing the file's actual bytes, which is what
// PdbRowWriter::commit() now does via a whole-file CRC32. This test exists
// so a regression back to metadata-only staleness detection fails here
// instead of silently reaching a user's real export.pdb.
//
// SEABASS_SOURCE_DIR is injected by CMakeLists.txt (CMAKE_SOURCE_DIR) so
// the fixture resolves regardless of the build directory's location --
// same convention as anonymized_fixture_integration_test.cpp.

namespace fs = std::filesystem;
using seabass::infrastructure::rekordbox::PdbRowWriter;

namespace
{

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// A fresh copy of the committed fixture per case, so neither case can
// observe the other's writes.
fs::path freshFixtureCopy(const fs::path &scratch)
{
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    fs::path source =
        fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox" / "rekordbox" / "export.pdb";
    assert(fs::exists(source));
    fs::path dest = scratch / "export.pdb";
    fs::copy_file(source, dest);
    return dest;
}

PdbRowWriter::TrackTextOverride titleOverride(const std::string &title)
{
    PdbRowWriter::TrackTextOverride override;
    override.title = title;
    return override;
}

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_pdb_row_writer_staleness_test";

    // The control case is not a formality -- it's what distinguishes "the
    // guard works" from "the guard refuses everything". A guard stuck
    // permanently on would make case 2 below pass while breaking every
    // real write in the app, so an over-firing regression has to fail
    // here.
    {
        fs::path pdbPath = freshFixtureCopy(scratch);
        const std::string pristine = readFile(pdbPath);

        PdbRowWriter writer(pdbPath.string());
        const bool edited = writer.overwriteTrackText(1, titleOverride("staleness probe control"));
        assert(edited);
        const bool committed = writer.commit();
        assert(committed);

        assert(readFile(pdbPath) != pristine);  // the edit really did land
        std::cout << "case 1 (no external interference: commit succeeds and writes) OK\n";
    }

    {
        fs::path pdbPath = freshFixtureCopy(scratch);

        PdbRowWriter writer(pdbPath.string());
        const bool edited = writer.overwriteTrackText(1, titleOverride("staleness probe stale"));
        assert(edited);

        // The external modification: same process, byte-identical length.
        // Flipping a single byte rather than rewriting the file wholesale
        // keeps this pinned to the case metadata checks cannot see -- if
        // it also changed the length, the cheap size comparison would
        // catch it and the checksum path would go untested.
        const std::string beforeTamper = readFile(pdbPath);
        std::string tampered = beforeTamper;
        const size_t flipAt = tampered.size() / 2;
        tampered[flipAt] = static_cast<char>(tampered[flipAt] ^ 0xFF);
        writeFile(pdbPath, tampered);
        assert(tampered.size() == beforeTamper.size());  // size genuinely unchanged
        assert(tampered != beforeTamper);                // but the bytes genuinely differ

        // Hoisted rather than asserted inline: under NDEBUG an
        // assert(!writer.commit()) would compile the commit() call itself
        // away, so the very write this test exists to catch would never
        // run. That is not hypothetical -- it is the exact NDEBUG bug
        // found elsewhere in this suite.
        const bool committed = writer.commit();
        assert(!committed);

        // The refusal has to be a real refusal: the tampered bytes must
        // still be on disk, not overwritten by the writer's stale buffer.
        assert(readFile(pdbPath) == tampered);
        std::cout << "case 2 (external same-length in-process rewrite: commit refused, disk untouched) OK\n";
    }

    fs::remove_all(scratch);
    std::cout << "all cases passed\n";
    return 0;
}
