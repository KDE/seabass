// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/real_file_sizes.hpp"

#include <algorithm>
#include <filesystem>
#include <map>

#include "application/path_key.hpp"

namespace seabass::application
{

namespace fs = std::filesystem;

namespace
{

// Same rule the cleanup page uses: a stray the planner refused to delete
// is in toRemove like any other non-survivor, but it is not on offer, so
// its bytes must not be promised.
bool heldBackStray(const domain::DuplicateCleanupPlan &plan, const domain::Track &track)
{
    return std::any_of(plan.unreferencedFilesHeldBack.begin(), plan.unreferencedFilesHeldBack.end(),
                        [&track](const domain::Track &held) { return held.sourceId == track.sourceId; });
}

}  // namespace

MeasuredFileSizes measureRealFileSizes(std::vector<domain::DuplicateCleanupPlan> &plans)
{
    MeasuredFileSizes measured;

    // path key -> size on disk, so a file named by several plans (or by
    // several catalogs' rows within one plan) is stat'd once and counted
    // once. Keyed the same way every other path comparison in this
    // codebase is, because exFAT and NTFS are case-insensitive and two
    // spellings of one path are one file.
    std::map<std::string, std::uintmax_t> sizeByPath;

    for (auto &plan : plans) {
        const std::string survivorKey = normalizedPathKey(plan.survivor.filePath);
        for (auto &doomed : plan.toRemove) {
            if (doomed.filePath.empty() || heldBackStray(plan, doomed)) {
                doomed.fileSizeBytes = 0;
                continue;
            }
            const std::string key = normalizedPathKey(doomed.filePath);
            // A copy sharing the survivor's path is not a file that goes
            // away -- the survivor keeps it. Counting it would promise
            // space that the cleanup, correctly, never frees.
            if (!survivorKey.empty() && key == survivorKey) {
                doomed.fileSizeBytes = 0;
                continue;
            }

            auto known = sizeByPath.find(key);
            if (known == sizeByPath.end()) {
                std::error_code ec;
                const std::uintmax_t size = fs::file_size(doomed.filePath, ec);
                if (ec) {
                    // Named by a catalog, absent from the stick. Removing
                    // its row frees nothing, so it contributes nothing.
                    ++measured.filesMissing;
                    known = sizeByPath.emplace(key, 0).first;
                } else {
                    ++measured.filesMeasured;
                    measured.reclaimableBytes += size;
                    known = sizeByPath.emplace(key, size).first;
                }
            }
            doomed.fileSizeBytes = static_cast<std::uint64_t>(known->second);
        }
    }
    return measured;
}

}  // namespace seabass::application
