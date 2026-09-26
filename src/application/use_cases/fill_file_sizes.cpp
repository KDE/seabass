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

std::optional<std::uint64_t> fileSizeOnDisk(const std::string &utf8Path)
{
    if (utf8Path.empty()) {
        return std::nullopt;
    }
    try {
        std::error_code ec;
        const auto onDisk = std::filesystem::file_size(pathFromUtf8(utf8Path), ec);
        if (!ec) {
            return static_cast<std::uint64_t>(onDisk);
        }
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

void fillFileSizes(std::vector<domain::Track> &tracks, CancellationToken cancel, ProgressReporter &progress)
{
    std::unordered_map<std::string, std::uint64_t> sizeByPath;
    size_t statted = 0;
    for (auto &track : tracks) {
        if (track.fileSizeBytes != 0 || track.filePath.empty()) {
            continue;
        }
        auto known = sizeByPath.find(track.filePath);
        if (known == sizeByPath.end()) {
            cancel.throwIfCancelled();
            known = sizeByPath.emplace(track.filePath, fileSizeOnDisk(track.filePath).value_or(0)).first;
            progress.tick(++statted);
        }
        track.fileSizeBytes = known->second;
    }
}

namespace
{

// dropMissingArtwork() over the rows `checked` accepts; the others keep
// their artworkPath untouched.
template <typename Checked>
void dropMissingArtworkWhere(std::vector<domain::Track> &tracks, const CancellationToken &cancel,
                             ProgressReporter &progress, Checked checked)
{
    std::unordered_map<std::string, bool> presentByPath;
    size_t looked = 0;
    for (auto &track : tracks) {
        if (track.artworkPath.empty() || !checked(track)) {
            continue;
        }
        auto known = presentByPath.find(track.artworkPath);
        if (known == presentByPath.end()) {
            cancel.throwIfCancelled();
            bool present = false;
            try {
                std::error_code ec;
                present = std::filesystem::exists(pathFromUtf8(track.artworkPath), ec);
            } catch (const std::exception &) {
            }
            known = presentByPath.emplace(track.artworkPath, present).first;
            progress.tick(++looked);
        }
        if (!known->second) {
            track.artworkPath.clear();
        }
    }
}

}  // namespace

void dropMissingArtwork(std::vector<domain::Track> &tracks, CancellationToken cancel, ProgressReporter &progress)
{
    dropMissingArtworkWhere(tracks, cancel, progress, [](const domain::Track &) { return true; });
}

void completeTracks(std::vector<domain::Track> &tracks, CancellationToken cancel, ProgressReporter &progress)
{
    fillFileSizes(tracks, cancel, progress);
    dropMissingArtworkWhere(tracks, cancel, progress, [](const domain::Track &track) {
        return track.format != "rekordbox";
    });
}

}  // namespace seabass::application
