// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/imported_track.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "domain/import_field_checks.hpp"

namespace seabass::domain
{

namespace
{

bool isBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
}

// The extension of the last name of `path`, lower case, without the dot.
std::string extensionOf(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    std::string extension(name.substr(dot + 1));
    for (char &c : extension) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return extension;
}

}  // namespace

std::string storableText(std::string_view text)
{
    while (!text.empty() && isBlank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isBlank(text.back())) {
        text.remove_suffix(1);
    }
    return isStorableText(text) ? std::string(text) : std::string();
}

ImportedTrack describeImportedTrack(const TrackTags &tags, const AudioFormat &audio, std::string_view stickPath,
                                    std::string_view date)
{
    if (!isCalendarDate(date)) {
        throw std::invalid_argument("\"" + std::string(date) + "\" is not a date as YYYY-MM-DD");
    }
    const std::size_t slash = stickPath.rfind('/');
    if (!isPlainStickPath(stickPath) || !isStorableText(stickPath)) {
        throw std::invalid_argument("\"" + std::string(stickPath) + "\" is not a path from the stick's root to a file");
    }
    ImportedTrack track;
    track.stickPath = std::string(stickPath);
    track.date = std::string(date);

    track.title = storableText(tags.title);
    if (track.title.empty()) {
        // The file's name without its extension. A name that is only an
        // extension (".flac") leaves nothing, and is the title as it is.
        const std::string_view name = stickPath.substr(slash + 1);
        const std::size_t dot = name.rfind('.');
        track.title = storableText(dot == std::string_view::npos ? name : name.substr(0, dot));
        if (track.title.empty()) {
            track.title = std::string(name);
        }
    }
    track.artist = storableText(tags.artist);
    track.album = storableText(tags.album);
    track.albumArtist = storableText(tags.albumArtist);
    track.genre = storableText(tags.genre);
    track.label = storableText(tags.label);
    track.remixer = storableText(tags.remixer);
    track.composer = storableText(tags.composer);
    track.key = storableText(tags.key);
    track.comment = storableText(tags.comment);
    track.bpm = tags.bpm > 0.0 && tags.bpm < 1000.0 ? tags.bpm : 0.0;
    track.year = tags.year > 0 && tags.year <= 9999 ? tags.year : 0;
    track.trackNumber = tags.trackNumber > 0 && tags.trackNumber <= 9999 ? tags.trackNumber : 0;

    const std::string extension = extensionOf(stickPath);
    const bool lossy = extension == "mp3" || extension == "m4a" || extension == "mp4" || extension == "ogg"
                       || extension == "opus";
    if (audio.durationSeconds > 0.0 && std::isfinite(audio.durationSeconds)) {
        track.durationSeconds = audio.durationSeconds;
        // The largest whole number below the length; a row's length field
        // ends at 65535 s, which the writers refuse past.
        track.lengthSeconds = static_cast<int>(std::min(std::ceil(audio.durationSeconds) - 1.0, 1.0e9));
    }
    track.bitrate = extension == "flac" || audio.bitrate < 0 ? 0 : audio.bitrate;
    track.bitDepth = lossy || audio.bitsPerSample <= 0 ? 16 : audio.bitsPerSample;
    track.sampleRateHz = audio.sampleRateHz > 0 ? audio.sampleRateHz : 0;
    track.fileSizeBytes = audio.fileSizeBytes;
    return track;
}

}  // namespace seabass::domain
