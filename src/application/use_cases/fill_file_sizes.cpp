// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/fill_file_sizes.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::application
{

void fillFileSizes(std::vector<domain::Track> &tracks)
{
    std::unordered_map<std::string, std::uint64_t> sizeByPath;
    for (auto &track : tracks) {
        if (track.fileSizeBytes != 0 || track.filePath.empty()) {
            continue;
        }
        auto known = sizeByPath.find(track.filePath);
        if (known == sizeByPath.end()) {
            std::uint64_t size = 0;
            // pathFromUtf8 can throw on Windows for bytes that are not
            // valid UTF-8 (a raw catalog column): that row stays unknown.
            try {
                std::error_code ec;
                const auto onDisk = std::filesystem::file_size(pathFromUtf8(track.filePath), ec);
                if (!ec) {
                    size = static_cast<std::uint64_t>(onDisk);
                }
            } catch (const std::exception &) {
            }
            known = sizeByPath.emplace(track.filePath, size).first;
        }
        track.fileSizeBytes = known->second;
    }
}

void dropMissingArtwork(std::vector<domain::Track> &tracks)
{
    std::unordered_map<std::string, bool> presentByPath;
    for (auto &track : tracks) {
        if (track.artworkPath.empty()) {
            continue;
        }
        auto known = presentByPath.find(track.artworkPath);
        if (known == presentByPath.end()) {
            bool present = false;
            try {
                std::error_code ec;
                present = std::filesystem::exists(pathFromUtf8(track.artworkPath), ec);
            } catch (const std::exception &) {
            }
            known = presentByPath.emplace(track.artworkPath, present).first;
        }
        if (!known->second) {
            track.artworkPath.clear();
        }
    }
}

}  // namespace seabass::application
