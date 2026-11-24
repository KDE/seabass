// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/taglib_tag_reader.hpp"

#include "infrastructure/paths/utf8_path.hpp"

#include <exception>
#include <filesystem>
#include <initializer_list>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>

namespace seabass::infrastructure::audio
{

namespace
{

// UTF-8, without the spaces and NULs taggers leave at either end.
std::string text(const TagLib::String &s)
{
    std::string out = s.isEmpty() ? std::string() : s.to8Bit(true);
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };
    while (!out.empty() && blank(out.back())) {
        out.pop_back();
    }
    size_t start = 0;
    while (start < out.size() && blank(out[start])) {
        ++start;
    }
    return out.substr(start);
}

// The first of `names` the file has a value under. Names are TagLib's
// property keys, upper case: the Vorbis comment's own field name, and
// what TagLib maps an ID3v2 frame or an MP4 atom to.
std::string property(const TagLib::PropertyMap &properties, std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        const auto found = properties.find(name);
        if (found == properties.end()) {
            continue;
        }
        for (const TagLib::String &value : found->second) {
            const std::string spelled = text(value);
            if (!spelled.empty()) {
                return spelled;
            }
        }
    }
    return {};
}

}  // namespace

namespace
{

std::optional<domain::TrackTags> readTags(const std::string &absoluteFilePath)
{
    // The path as TagLibMetadataProbe hands it over, for the same reason.
    // No audio properties: the import reads those through the probe.
    const std::filesystem::path path = pathFromUtf8(absoluteFilePath);
    TagLib::FileRef file(path.c_str(), false);
    // The isValid() half is TagLibMetadataProbe's guard, kept although no
    // file of taglib_tag_reader_test trips it alone: a missing file, a
    // text file and garbage named .flac all come back null already.
    if (file.isNull() || file.file() == nullptr || !file.file()->isValid()) {
        return std::nullopt;
    }

    domain::TrackTags tags;
    if (const TagLib::Tag *tag = file.tag()) {
        tags.title = text(tag->title());
        tags.artist = text(tag->artist());
        tags.album = text(tag->album());
        tags.genre = text(tag->genre());
        // A year a tagger can have meant; 0 is TagLib's "none".
        const unsigned int year = tag->year();
        tags.year = year >= 1000 && year <= 9999 ? static_cast<int>(year) : 0;
        const unsigned int track = tag->track();
        tags.trackNumber = track <= 9999 ? static_cast<int>(track) : 0;
    }

    const TagLib::PropertyMap properties = file.file()->properties();
    // Not Tag::comment(): for ID3v2 that falls back to the first COMM
    // frame of any description, and a file from iTunes then "comments"
    // its own volume data (iTunNORM). The property is the comment
    // without a description only.
    tags.comment = property(properties, {"COMMENT", "DESCRIPTION"});
    tags.albumArtist = property(properties, {"ALBUMARTIST", "ALBUM ARTIST"});
    tags.composer = property(properties, {"COMPOSER"});
    tags.label = property(properties, {"LABEL", "PUBLISHER", "ORGANIZATION"});
    tags.remixer = property(properties, {"REMIXER"});
    tags.key = property(properties, {"INITIALKEY", "KEY"});
    tags.bpm = domain::bpmFromTag(property(properties, {"BPM", "TEMPO"}));
    return tags;
}

}  // namespace

std::optional<domain::TrackTags> TagLibTagReader::read(const std::string &absoluteFilePath)
{
    try {
        return readTags(absoluteFilePath);
    } catch (const std::exception &) {
        // A path that is not UTF-8 (pathFromUtf8 on Windows), or TagLib
        // out of memory on a damaged file: no answer, as promised.
        return std::nullopt;
    }
}

}  // namespace seabass::infrastructure::audio
