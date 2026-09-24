// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/embedded_artwork.hpp"

#include "infrastructure/paths/utf8_path.hpp"

#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4file.h>
#include <taglib/mpegfile.h>
#include <taglib/tfile.h>
#include <taglib/tstring.h>
#include <taglib/xiphcomment.h>

#include <algorithm>
#include <filesystem>

namespace seabass::infrastructure::audio
{

namespace
{

std::string toStdString(const TagLib::ByteVector &bytes)
{
    return std::string(bytes.data(), bytes.size());
}

// A file can carry several pictures -- front cover, back cover, a photo
// of the artist. A player shows the front cover, so that is what a
// rebuilt library should hold; anything else only if there is no front.
std::string fromId3(TagLib::ID3v2::Tag *tag)
{
    if (tag == nullptr) {
        return {};
    }
    const TagLib::ID3v2::FrameList &frames = tag->frameList("APIC");
    std::string fallback;
    for (const TagLib::ID3v2::Frame *frame : frames) {
        const auto *picture = dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame *>(frame);
        if (picture == nullptr || picture->picture().isEmpty()) {
            continue;
        }
        if (picture->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) {
            return toStdString(picture->picture());
        }
        if (fallback.empty()) {
            fallback = toStdString(picture->picture());
        }
    }
    return fallback;
}

std::string fromPictures(const TagLib::List<TagLib::FLAC::Picture *> &pictures)
{
    std::string fallback;
    for (const TagLib::FLAC::Picture *picture : pictures) {
        if (picture == nullptr || picture->data().isEmpty()) {
            continue;
        }
        if (picture->type() == TagLib::FLAC::Picture::FrontCover) {
            return toStdString(picture->data());
        }
        if (fallback.empty()) {
            fallback = toStdString(picture->data());
        }
    }
    return fallback;
}

}  // namespace

std::string readEmbeddedArtwork(const std::string &audioFile)
{
    if (audioFile.empty()) {
        return {};
    }
    // By extension rather than by TagLib::FileRef: the picture lives in a
    // format-specific place, so the concrete type is needed anyway, and
    // TagLib 1.x has no format-agnostic way to ask for it.
    std::string lower = audioFile;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto endsWith = [&lower](std::string_view suffix) {
        return lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    // TagLib::FileName is const char* on POSIX and, on Windows, a class
    // whose const char* constructor reads the ANSI code page; the
    // fs::path's c_str() is the wchar_t* its other constructor takes.
    const std::filesystem::path path = pathFromUtf8(audioFile);

    if (endsWith(".mp3") || endsWith(".aiff") || endsWith(".aif") || endsWith(".wav")) {
        TagLib::MPEG::File file(path.c_str(), false);
        if (file.isValid()) {
            return fromId3(file.ID3v2Tag());
        }
        return {};
    }
    if (endsWith(".flac")) {
        TagLib::FLAC::File file(path.c_str(), false);
        if (!file.isValid()) {
            return {};
        }
        std::string picture = fromPictures(file.pictureList());
        if (picture.empty() && file.hasID3v2Tag()) {
            picture = fromId3(file.ID3v2Tag());
        }
        return picture;
    }
    if (endsWith(".m4a") || endsWith(".mp4") || endsWith(".m4b")) {
        TagLib::MP4::File file(path.c_str(), false);
        if (!file.isValid() || file.tag() == nullptr) {
            return {};
        }
        const TagLib::MP4::ItemMap &items = file.tag()->itemMap();
        const auto cover = items.find("covr");
        if (cover == items.end()) {
            return {};
        }
        const TagLib::MP4::CoverArtList covers = cover->second.toCoverArtList();
        for (const TagLib::MP4::CoverArt &art : covers) {
            if (!art.data().isEmpty()) {
                return toStdString(art.data());
            }
        }
        return {};
    }
    if (endsWith(".ogg") || endsWith(".opus")) {
        TagLib::Ogg::XiphComment comment;
        TagLib::FLAC::File file(path.c_str(), false);
        if (file.isValid() && file.hasXiphComment() && file.xiphComment() != nullptr) {
            return fromPictures(file.xiphComment()->pictureList());
        }
        return {};
    }
    return {};
}

bool hasEmbeddedArtwork(const std::string &audioFile)
{
    return !readEmbeddedArtwork(audioFile).empty();
}

}  // namespace seabass::infrastructure::audio
