// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/taglib_metadata_probe.hpp"

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/mpegproperties.h>
#include <taglib/tag.h>
#include <taglib/tstring.h>

namespace seabass::infrastructure::audio
{

namespace
{

std::string toStdString(const TagLib::String &s)
{
    return s.isEmpty() ? std::string() : s.to8Bit(true);  // true == UTF-8
}

// True when TagLib had to compute the length from stream size and
// bitrate instead of reading a real one. That is exactly the MPEG case
// with no Xing/VBRI header: TagLib::XingHeader covers both, so a null
// xingHeader() is the precise test. Every other container we meet
// (MP4/M4A, FLAC, Ogg, WAV) reports a length from its own header, which
// is exact.
bool durationIsEstimated(TagLib::AudioProperties *properties)
{
    auto *mpeg = dynamic_cast<TagLib::MPEG::Properties *>(properties);
    return mpeg != nullptr && mpeg->xingHeader() == nullptr;
}

}  // namespace

std::optional<application::FileMetadata> TagLibMetadataProbe::read(const std::string &absoluteFilePath)
{
    // readAudioProperties = true, Average style: Accurate would scan the
    // whole file, which on a USB stick costs far more than it buys --
    // and it still cannot turn a headerless VBR length into an exact
    // one, which is what durationIsEstimated exists to flag.
    TagLib::FileRef file(absoluteFilePath.c_str(), true, TagLib::AudioProperties::Average);
    if (file.isNull() || file.file() == nullptr || !file.file()->isValid()) {
        return std::nullopt;
    }

    application::FileMetadata metadata;

    if (TagLib::Tag *tag = file.tag()) {
        metadata.title = toStdString(tag->title());
        metadata.artist = toStdString(tag->artist());
        metadata.album = toStdString(tag->album());
    }

    TagLib::AudioProperties *properties = file.audioProperties();
    const int lengthMs = properties != nullptr ? properties->lengthInMilliseconds() : 0;
    if (lengthMs <= 0) {
        // TagLib::File::isValid() above only says "opened and parsed far
        // enough to hand back an object" -- a four-byte text file named
        // .mp3 passes it and yields properties with a zero length. A
        // file we cannot get a playing length from is useless to every
        // caller here (DuplicateTrackFinder refuses to group a track
        // with no duration at all), so report it the same way as a file
        // that could not be opened rather than handing back a Track-
        // shaped object full of zeroes that reads as real data.
        return std::nullopt;
    }

    metadata.durationSeconds = lengthMs / 1000.0;
    metadata.bitrate = properties->bitrate();
    metadata.sampleRate = properties->sampleRate();
    metadata.durationIsEstimated = durationIsEstimated(properties);

    return metadata;
}

}  // namespace seabass::infrastructure::audio
