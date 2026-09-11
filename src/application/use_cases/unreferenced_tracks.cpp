// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/unreferenced_tracks.hpp"

#include <algorithm>
#include <filesystem>

namespace seabass::application
{

namespace
{

namespace fs = std::filesystem;

// Same first step as find_unreferenced_files.cpp's normalize(), and for
// the same reason: std::filesystem only treats '\' as a separator on
// Windows, so a stick written on Windows and read on Linux would
// otherwise yield a "filename" that is the whole path.
std::string fileNameOf(const std::string &path)
{
    std::string slashed = path;
    std::replace(slashed.begin(), slashed.end(), '\\', '/');
    return fs::path(slashed).filename().string();
}

}  // namespace

std::vector<domain::Track> unreferencedFilesAsTracks(const std::vector<AudioFileOnDisk> &files,
                                                       TrackMetadataProbe &probe, MetadataCachePort *cache,
                                                       const CancellationToken &cancel)
{
    std::vector<domain::Track> tracks;
    tracks.reserve(files.size());

    for (const auto &file : files) {
        cancel.throwIfCancelled();

        std::optional<FileMetadata> metadata;
        if (cache != nullptr) {
            metadata = cache->lookup(file.filePath);
        }
        if (!metadata.has_value()) {
            metadata = probe.read(file.filePath);
            if (metadata.has_value() && cache != nullptr) {
                cache->store(file.filePath, *metadata);
            }
        }
        if (!metadata.has_value()) {
            continue;  // unreadable: it stays on the stick, unproposed
        }

        domain::Track track;
        // No row means no row id. The path is what identifies this copy,
        // and it is what the deletion step needs anyway.
        track.sourceId = file.filePath;
        track.format = "disk";
        track.isUnreferenced = true;
        track.title = metadata->title;
        track.artist = metadata->artist;
        track.filename = fileNameOf(file.filePath);
        track.filePath = file.filePath;
        track.fileSizeBytes = file.fileSizeBytes;
        track.bitrate = metadata->bitrate;
        track.durationSeconds = metadata->durationSeconds;
        track.durationIsEstimated = metadata->durationIsEstimated;
        tracks.push_back(std::move(track));
    }

    return tracks;
}

}  // namespace seabass::application
