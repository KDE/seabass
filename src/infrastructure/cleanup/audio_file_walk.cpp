// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/cleanup/audio_file_walk.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

#include "infrastructure/long_paths.hpp"

namespace seabass::infrastructure::cleanup
{

namespace
{

namespace fs = std::filesystem;

// rekordbox and Engine between them import these. Measured on RV2 the
// whole stick is .mp3 and .m4a, but a stick from a different DJ will
// carry WAV and AIFF sets, and Engine also takes FLAC and Ogg.
constexpr std::array<const char *, 11> AudioExtensions = {
    ".mp3", ".m4a", ".mp4", ".aac", ".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".ogg",
};

std::string lowerExtension(const std::string &path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    // A dot in a directory name is not an extension.
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos && dot < slash) {
        return {};
    }
    std::string ext = path.substr(dot);
    for (auto &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

}  // namespace

bool isAudioExtension(const std::string &path)
{
    const std::string ext = lowerExtension(path);
    if (ext.empty()) {
        return false;
    }
    return std::find_if(AudioExtensions.begin(), AudioExtensions.end(),
                        [&ext](const char *known) { return ext == known; }) != AudioExtensions.end();
}

AudioFileWalkResult walkAudioFiles(const std::string &root, const application::CancellationToken &cancel)
{
    AudioFileWalkResult result;
    if (root.empty()) {
        return result;
    }

    std::error_code ec;
    if (!fs::exists(root, ec) || ec) {
        result.incomplete = true;
        return result;
    }

    // Explicit stack rather than recursion: a deep tree on a real stick
    // should cost heap, not call frames.
    std::vector<fs::path> pending{fs::path(root)};
    while (!pending.empty()) {
        cancel.throwIfCancelled();

        const fs::path directory = pending.back();
        pending.pop_back();

        std::error_code openEc;
        DirectoryReader reader(directory, openEc);
        if (openEc) {
            result.incomplete = true;
            continue;
        }
        ++result.directoriesVisited;

        fs::path child;
        std::error_code nextEc;
        while (reader.next(child, nextEc)) {
            cancel.throwIfCancelled();

            std::error_code statEc;
            // symlink_status, not status: a symlink must be identified as
            // one rather than followed to whatever it points at.
            const fs::file_status status = fs::symlink_status(child, statEc);
            if (statEc) {
                result.incomplete = true;
                continue;
            }
            if (fs::is_symlink(status)) {
                continue;  // never followed, never reported
            }
            if (fs::is_directory(status)) {
                pending.push_back(child);
                continue;
            }
            if (!fs::is_regular_file(status)) {
                continue;
            }

            const std::string path = child.generic_string();
            if (!isAudioExtension(path)) {
                continue;
            }

            application::AudioFileOnDisk file;
            file.filePath = path;
            std::error_code sizeEc;
            const auto size = fs::file_size(child, sizeEc);
            // fs::file_size reports failure as (uintmax_t)-1, so a caller
            // adding it up without checking turns one bad file into a
            // nonsense total. Leave it at 0 instead.
            file.fileSizeBytes = sizeEc ? 0 : static_cast<std::uint64_t>(size);
            result.files.push_back(std::move(file));
        }
        if (nextEc) {
            result.incomplete = true;
        }
    }

    return result;
}

}  // namespace seabass::infrastructure::cleanup
