// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What a cleanup would really free, measured on disk rather than believed
// from a catalog. The catalogs cannot answer this: rekordbox and Engine
// record no file size at all, so every figure derived from them was zero.
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/real_file_sizes.hpp"

#include "scratch_path.hpp"

using namespace seabass;
namespace fs = std::filesystem;

namespace
{

fs::path scratchDir()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_real_file_sizes_test";
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

fs::path fileOfSize(const fs::path &dir, const std::string &name, std::size_t bytes)
{
    fs::path path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
    out.close();
    return path;
}

domain::Track copyAt(const std::string &sourceId, const fs::path &path, std::uint64_t claimedSize)
{
    domain::Track t;
    t.sourceId = sourceId;
    t.format = "rekordbox";
    t.filePath = path.string();
    t.fileSizeBytes = claimedSize;
    return t;
}

}  // namespace

int main()
{
    const fs::path dir = scratchDir();

    // The core case: the catalog claims nothing (rekordbox and Engine
    // really do report 0), the disk knows.
    {
        auto keep = fileOfSize(dir, "keep.mp3", 500);
        auto drop = fileOfSize(dir, "drop.mp3", 1200);

        domain::DuplicateCleanupPlan plan;
        plan.survivor = copyAt("keep", keep, 0);
        plan.toRemove = {copyAt("drop", drop, 0)};
        std::vector<domain::DuplicateCleanupPlan> plans{plan};

        auto measured = application::measureRealFileSizes(plans);
        assert(measured.filesMeasured == 1);
        assert(measured.filesMissing == 0);
        assert(measured.reclaimableBytes == 1200);
        // Written back, because every figure the page shows comes from it.
        assert(plans[0].toRemove[0].fileSizeBytes == 1200);
        std::cout << "case 1 (a catalog claiming zero still yields the real size) OK\n";
    }

    // A catalog that lies about the size loses to the disk. OneLibrary is
    // the only format that records one, and it is still only a claim.
    {
        auto drop = fileOfSize(dir, "lied.mp3", 700);
        domain::DuplicateCleanupPlan plan;
        plan.survivor = copyAt("keep", dir / "keep.mp3", 0);
        plan.toRemove = {copyAt("drop", drop, 999999)};
        std::vector<domain::DuplicateCleanupPlan> plans{plan};

        auto measured = application::measureRealFileSizes(plans);
        assert(measured.reclaimableBytes == 700);
        assert(plans[0].toRemove[0].fileSizeBytes == 700);
        std::cout << "case 2 (the disk overrules a catalog's claim) OK\n";
    }

    // A row naming a file that is not on the stick frees nothing. The old
    // computation would have added the catalog's number and promised
    // space that cannot appear.
    {
        domain::DuplicateCleanupPlan plan;
        plan.survivor = copyAt("keep", dir / "keep.mp3", 0);
        plan.toRemove = {copyAt("ghost", dir / "not-here.mp3", 4242)};
        std::vector<domain::DuplicateCleanupPlan> plans{plan};

        auto measured = application::measureRealFileSizes(plans);
        assert(measured.filesMissing == 1);
        assert(measured.filesMeasured == 0);
        assert(measured.reclaimableBytes == 0);
        assert(plans[0].toRemove[0].fileSizeBytes == 0);
        std::cout << "case 3 (a row pointing at nothing frees nothing) OK\n";
    }

    // Held back by the planner: listed in toRemove, never deleted, so its
    // bytes are not on offer.
    {
        auto drop = fileOfSize(dir, "heldback.mp3", 3000);
        domain::DuplicateCleanupPlan plan;
        plan.survivor = copyAt("keep", dir / "keep.mp3", 0);
        auto stray = copyAt("stray", drop, 0);
        stray.isUnreferenced = true;
        plan.toRemove = {stray};
        plan.unreferencedFilesHeldBack = {stray};
        std::vector<domain::DuplicateCleanupPlan> plans{plan};

        auto measured = application::measureRealFileSizes(plans);
        assert(measured.reclaimableBytes == 0);
        assert(plans[0].toRemove[0].fileSizeBytes == 0);
        std::cout << "case 4 (a held-back stray promises nothing) OK\n";
    }

    // One file, several plans and several catalog rows: counted once.
    // Without this the totals inflate exactly as much as the library is
    // duplicated across formats, which is the whole stick.
    {
        auto drop = fileOfSize(dir, "shared.mp3", 800);
        domain::DuplicateCleanupPlan a;
        a.survivor = copyAt("keepA", dir / "keep.mp3", 0);
        a.toRemove = {copyAt("dropRb", drop, 0)};
        domain::DuplicateCleanupPlan b;
        b.survivor = copyAt("keepB", dir / "keep.mp3", 0);
        b.toRemove = {copyAt("dropEn", drop, 0)};
        std::vector<domain::DuplicateCleanupPlan> plans{a, b};

        auto measured = application::measureRealFileSizes(plans);
        assert(measured.filesMeasured == 1);
        assert(measured.reclaimableBytes == 800);
        // Both rows still report the file's real size for display; only
        // the total refuses to count it twice.
        assert(plans[0].toRemove[0].fileSizeBytes == 800);
        assert(plans[1].toRemove[0].fileSizeBytes == 800);
        std::cout << "case 5 (one file named by two plans is counted once) OK\n";
    }

    // A copy sharing the survivor's own path is not space that is freed:
    // the survivor keeps that file.
    {
        auto keep = fileOfSize(dir, "same.mp3", 900);
        domain::DuplicateCleanupPlan plan;
        plan.survivor = copyAt("keep", keep, 0);
        plan.toRemove = {copyAt("otherRow", keep, 0)};
        std::vector<domain::DuplicateCleanupPlan> plans{plan};

        auto measured = application::measureRealFileSizes(plans);
        assert(measured.reclaimableBytes == 0);
        assert(plans[0].toRemove[0].fileSizeBytes == 0);
        std::cout << "case 6 (a row for the survivor's own file frees nothing) OK\n";
    }

    fs::remove_all(dir);
    std::cout << "all real_file_sizes_test cases passed\n";
    return 0;
}
