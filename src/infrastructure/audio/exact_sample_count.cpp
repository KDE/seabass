// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/exact_sample_count.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

#include <taglib/aifffile.h>
#include <taglib/flacfile.h>
#include <taglib/mpegfile.h>
#include <taglib/wavfile.h>
#include <taglib/xingheader.h>

#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

using Source = ExactSampleCount::Source;

std::optional<ExactSampleCount> counted(std::uint64_t samples, int rate, Source source)
{
    if (samples == 0 || rate <= 0) {
        return std::nullopt;
    }
    return ExactSampleCount{samples, static_cast<double>(rate), source};
}

std::string lowerExtension(const std::filesystem::path &path)
{
    std::string extension = pathToUtf8(path.extension());
    for (char &c : extension) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return extension;
}

std::uint64_t bigEndian(std::string_view bytes, std::size_t at, int width)
{
    std::uint64_t v = 0;
    for (int i = 0; i < width; ++i) {
        v = (v << 8) | static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(i)]);
    }
    return v;
}

// One ISO box: its type and its payload. Iterates the boxes of `bytes`.
struct Box
{
    std::string_view type;
    std::string_view payload;
};

std::vector<Box> boxesOf(std::string_view bytes)
{
    std::vector<Box> out;
    std::size_t at = 0;
    while (at + 8 <= bytes.size()) {
        std::uint64_t size = bigEndian(bytes, at, 4);
        std::size_t header = 8;
        if (size == 1) {
            if (at + 16 > bytes.size()) {
                break;
            }
            size = bigEndian(bytes, at + 8, 8);
            header = 16;
        } else if (size == 0) {
            size = bytes.size() - at;
        }
        if (size < header || size > bytes.size() - at) {
            break;  // a box that does not fit: nothing after it is believed
        }
        out.push_back({bytes.substr(at + 4, 4), bytes.substr(at + header, static_cast<std::size_t>(size) - header)});
        at += static_cast<std::size_t>(size);
    }
    return out;
}

std::optional<std::string_view> child(std::string_view payload, std::string_view type)
{
    for (const Box &box : boxesOf(payload)) {
        if (box.type == type) {
            return box.payload;
        }
    }
    return std::nullopt;
}

// One trak: its handler, its media timescale and duration, and the sample
// rate of its first sample entry.
std::optional<ExactSampleCount> soundTrack(std::string_view trak)
{
    const auto mdia = child(trak, "mdia");
    if (!mdia) {
        return std::nullopt;
    }
    const auto hdlr = child(*mdia, "hdlr");
    if (!hdlr || hdlr->size() < 12 || hdlr->substr(8, 4) != "soun") {
        return std::nullopt;
    }
    const auto mdhd = child(*mdia, "mdhd");
    if (!mdhd || mdhd->empty()) {
        return std::nullopt;
    }
    std::uint64_t timescale = 0;
    std::uint64_t duration = 0;
    if ((*mdhd)[0] == 1) {
        if (mdhd->size() < 32) {
            return std::nullopt;
        }
        timescale = bigEndian(*mdhd, 20, 4);
        duration = bigEndian(*mdhd, 24, 8);
    } else {
        if (mdhd->size() < 20) {
            return std::nullopt;
        }
        timescale = bigEndian(*mdhd, 12, 4);
        duration = bigEndian(*mdhd, 16, 4);
    }
    if (timescale == 0 || duration == 0 || duration == 0xffffffffu) {
        return std::nullopt;
    }
    // The sample entry's rate (16.16 fixed point): the timescale is the
    // rate in every file seen, but not by rule.
    std::uint64_t rate = timescale;
    if (const auto minf = child(*mdia, "minf")) {
        if (const auto stbl = child(*minf, "stbl")) {
            if (const auto stsd = child(*stbl, "stsd"); stsd && stsd->size() >= 8 + 8 + 28) {
                const std::uint64_t entryRate = bigEndian(*stsd, 8 + 8 + 24, 4) >> 16;
                if (entryRate > 0) {
                    rate = entryRate;
                }
            }
        }
    }
    std::uint64_t samples = duration;
    if (rate != timescale) {
        // Only a count that comes out whole is a count.
        if ((duration * rate) % timescale != 0) {
            return std::nullopt;
        }
        samples = duration * rate / timescale;
    }
    return counted(samples, static_cast<int>(rate), Source::Mp4TrackDuration);
}

// The moov box of a file, found by walking the top-level boxes from disk
// (it can sit after a large mdat), at most 64 MB.
std::optional<std::string> moovOf(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::uint64_t at = 0;
    for (int boxes = 0; boxes < 4096; ++boxes) {
        char header[16];
        in.seekg(static_cast<std::streamoff>(at));
        if (!in.read(header, 8)) {
            return std::nullopt;
        }
        const std::string_view h(header, 8);
        std::uint64_t size = bigEndian(h, 0, 4);
        std::uint64_t headerSize = 8;
        if (size == 1) {
            if (!in.read(header + 8, 8)) {
                return std::nullopt;
            }
            size = bigEndian(std::string_view(header, 16), 8, 8);
            headerSize = 16;
        }
        if (size != 0 && size < headerSize) {
            return std::nullopt;
        }
        if (h.substr(4, 4) == "moov") {
            if (size == 0 || size - headerSize > 64u * 1024u * 1024u) {
                return std::nullopt;
            }
            std::string moov(static_cast<std::size_t>(size), '\0');
            in.seekg(static_cast<std::streamoff>(at));
            if (!in.read(moov.data(), static_cast<std::streamsize>(size))) {
                return std::nullopt;
            }
            return moov;
        }
        if (size == 0) {
            return std::nullopt;
        }
        at += size;
    }
    return std::nullopt;
}

}  // namespace

std::optional<ExactSampleCount> mp4SampleCountOf(std::string_view fileBytes)
{
    const auto moov = child(fileBytes, "moov");
    if (!moov) {
        return std::nullopt;
    }
    for (const Box &box : boxesOf(*moov)) {
        if (box.type == "trak") {
            if (auto count = soundTrack(box.payload)) {
                return count;
            }
        }
    }
    return std::nullopt;
}

std::optional<ExactSampleCount> readExactSampleCount(const std::string &absoluteFilePath)
{
    // TagLib::FileName: const char* on POSIX, the wide path on Windows
    // (see TagLibMetadataProbe::read()).
    const std::filesystem::path path = pathFromUtf8(absoluteFilePath);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    const std::string extension = lowerExtension(path);
    if (extension == ".flac") {
        TagLib::FLAC::File file(path.c_str(), true, TagLib::AudioProperties::Average);
        const auto *p = file.isValid() ? file.audioProperties() : nullptr;
        return p ? counted(p->sampleFrames(), p->sampleRate(), Source::FlacStreamInfo) : std::nullopt;
    }
    if (extension == ".wav") {
        TagLib::RIFF::WAV::File file(path.c_str(), true, TagLib::AudioProperties::Average);
        const auto *p = file.isValid() ? file.audioProperties() : nullptr;
        // 1 PCM, 3 IEEE float, 0xFFFE extensible: a frame is channels
        // times the sample's bytes. Anything compressed has no such rule.
        if (p == nullptr || (p->format() != 1 && p->format() != 3 && p->format() != 0xFFFE)) {
            return std::nullopt;
        }
        return counted(p->sampleFrames(), p->sampleRate(), Source::WavData);
    }
    if (extension == ".aif" || extension == ".aiff") {
        TagLib::RIFF::AIFF::File file(path.c_str(), true, TagLib::AudioProperties::Average);
        const auto *p = file.isValid() ? file.audioProperties() : nullptr;
        return p ? counted(p->sampleFrames(), p->sampleRate(), Source::AiffComm) : std::nullopt;
    }
    if (extension == ".m4a" || extension == ".mp4") {
        const auto moov = moovOf(path);
        return moov ? mp4SampleCountOf(*moov) : std::nullopt;
    }
    if (extension == ".mp3") {
        TagLib::MPEG::File file(path.c_str(), true, TagLib::AudioProperties::Average);
        const auto *p = file.isValid() ? file.audioProperties() : nullptr;
        if (p == nullptr || p->xingHeader() == nullptr || p->xingHeader()->totalFrames() == 0 || p->layer() != 3) {
            return std::nullopt;
        }
        const std::uint64_t perFrame = p->version() == TagLib::MPEG::Header::Version1 ? 1152 : 576;
        return counted(static_cast<std::uint64_t>(p->xingHeader()->totalFrames()) * perFrame, p->sampleRate(),
                       Source::Mp3InfoFrames);
    }
    return std::nullopt;
}

}  // namespace seabass::infrastructure::audio
