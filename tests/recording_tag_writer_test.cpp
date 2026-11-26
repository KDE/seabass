// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The tags and cover of a kept recording: QtCoverArtPreparer and
// TagLibRecordingTagWriter, on a FLAC the encoder made from a WAV this
// test writes:
//
//   - a large picture comes out as a baseline JPEG of at most 1200 pixels
//     on its longer side and 500 KB, its shape kept; a small one is not
//     scaled up; a transparent one is laid on white; bytes that are no
//     picture are refused;
//   - artist, title, album, genre, year and the front cover, text outside
//     ASCII included, read back through TagLib (here, not through the
//     writer) as they were written, exactly one value each;
//   - writing again replaces them, and fields left empty are gone;
//   - the audio is untouched: the same STREAMINFO (with its MD5), the
//     same audio frames byte for byte, and it decodes to the signal;
//   - a file that is not a FLAC, a folder in the way of the copy: a
//     failure, the file left byte for byte as it was.
//
//   recording_tag_writer_test SCRATCH_DIR
//
// No encoding backend or no JPEG writer is a FAIL, not a skip.

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/xiphcomment.h>

#include "application/ports/cover_art_preparer.hpp"
#include "infrastructure/audio/flac_stream_info.hpp"
#include "infrastructure/audio/qt_cover_art_preparer.hpp"
#include "infrastructure/audio/qt_flac_recording_encoder.hpp"
#include "infrastructure/audio/taglib_recording_tag_writer.hpp"
#include "recording_test_support.hpp"

using namespace seabass;
using infrastructure::audio::QtCoverArtPreparer;
using infrastructure::audio::QtFlacRecordingEncoder;
using infrastructure::audio::RecordingTagTempSuffix;
using infrastructure::audio::TagLibRecordingTagWriter;
namespace fs = std::filesystem;

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

std::string readAll(const std::string &path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string encoded(const QImage &image, const char *format)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// A photo-like picture: gradients under a little noise.
QImage photo(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    std::uint32_t seed = 12345;
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            seed = seed * 1664525U + 1013904223U;
            const int noise = static_cast<int>((seed >> 24) % 16);
            line[x] = qRgb((x * 255 / width + noise) % 256, (y * 255 / height + noise) % 256, ((x + y) % 256));
        }
    }
    return image;
}

// Whether a JPEG is baseline: a SOF0 frame and no progressive SOF2.
bool baselineJpeg(const std::string &jpeg)
{
    if (jpeg.size() < 4 || static_cast<unsigned char>(jpeg[0]) != 0xFF || static_cast<unsigned char>(jpeg[1]) != 0xD8) {
        return false;
    }
    bool sof0 = false;
    // Markers up to the start of the scan, segment by segment.
    for (std::size_t at = 2; at + 4 <= jpeg.size();) {
        if (static_cast<unsigned char>(jpeg[at]) != 0xFF) {
            return false;
        }
        const auto marker = static_cast<unsigned char>(jpeg[at + 1]);
        if (marker == 0xC2) {
            return false;
        }
        if (marker == 0xC0) {
            sof0 = true;
        }
        if (marker == 0xDA) {
            break;
        }
        const std::size_t length = (static_cast<unsigned char>(jpeg[at + 2]) << 8) | static_cast<unsigned char>(jpeg[at + 3]);
        at += 2 + length;
    }
    return sof0;
}

void covers()
{
    QtCoverArtPreparer preparer;
    // A large picture, as from a phone.
    {
        const std::string png = encoded(photo(3000, 2000), "png");
        check(png.size() > application::MaxCoverBytes, "large cover: the source is over the limit");
        const application::CoverArtResult result = preparer.prepare(png);
        check(result.cover.has_value(), "large cover: prepared (" + result.error + ")");
        if (result.cover) {
            const application::CoverArt &cover = *result.cover;
            check(cover.width == 1200 && cover.height == 800,
                  "large cover: scaled to 1200 x 800, got " + std::to_string(cover.width) + " x "
                      + std::to_string(cover.height));
            check(cover.jpeg.size() <= application::MaxCoverBytes,
                  "large cover: at most 500 KB, is " + std::to_string(cover.jpeg.size()));
            check(baselineJpeg(cover.jpeg), "large cover: a baseline JPEG");
            const QImage back = QImage::fromData(QByteArray(cover.jpeg.data(), static_cast<qsizetype>(cover.jpeg.size())),
                                                 "jpeg");
            check(back.width() == 1200 && back.height() == 800, "large cover: the JPEG reads back at its size");
        }
    }
    // Tall: the longer side is the height.
    {
        const application::CoverArtResult result = preparer.prepare(encoded(photo(900, 2400), "jpeg"));
        check(result.cover && result.cover->height == 1200 && result.cover->width == 450,
              "tall cover: 450 x 1200");
    }
    // Small and transparent: not scaled up, laid on white.
    {
        QImage clear(300, 200, QImage::Format_ARGB32);
        clear.fill(Qt::transparent);
        const application::CoverArtResult result = preparer.prepare(encoded(clear, "png"));
        check(result.cover && result.cover->width == 300 && result.cover->height == 200,
              "small cover: kept at 300 x 200");
        if (result.cover) {
            const QImage back = QImage::fromData(
                QByteArray(result.cover->jpeg.data(), static_cast<qsizetype>(result.cover->jpeg.size())), "jpeg");
            const QColor pixel = back.pixelColor(150, 100);
            check(pixel.red() > 245 && pixel.green() > 245 && pixel.blue() > 245,
                  "small cover: transparent became white");
            check(baselineJpeg(result.cover->jpeg), "small cover: a baseline JPEG");
        }
    }
    // Not a picture.
    {
        const application::CoverArtResult result = preparer.prepare("this is not a picture");
        check(!result.cover && !result.error.empty(), "no picture: refused with a reason");
    }
}

struct Audio
{
    std::array<std::uint8_t, 34> streamInfo{};
    std::string frames;
};

Audio audioOf(const std::string &flac)
{
    Audio audio;
    const auto info = infrastructure::audio::readFlacStreamInfo(pathFromUtf8(flac));
    if (info) {
        audio.streamInfo = info->raw;
        audio.frames = readAll(flac).substr(static_cast<std::size_t>(info->audioOffset));
    }
    return audio;
}

std::string field(TagLib::Ogg::XiphComment *comment, const char *name)
{
    if (comment == nullptr || !comment->contains(name)) {
        return "<absent>";
    }
    const TagLib::StringList values = comment->fieldListMap()[name];
    if (values.size() != 1) {
        return "<" + std::to_string(values.size()) + " values>";
    }
    return values.front().to8Bit(true);
}

void tags(const std::string &dir)
{
    const recording_test::WavSpec spec{44100, 2, 16, 44100 * 3};
    const std::string wav = dir + "/tagged.wav";
    const std::string flac = dir + "/Unicode-\xC3\xA9\xC3\x9F set.flac";
    recording_test::writeWav(wav, spec);
    QtFlacRecordingEncoder encoder;
    const auto made = encoder.encode(wav, flac, {});
    if (!made.encoded) {
        check(false, "tags: the FLAC to tag could not be made: " + made.error);
        return;
    }
    const Audio untagged = audioOf(flac);
    check(!untagged.frames.empty(), "tags: the untagged FLAC's audio is found");

    QtCoverArtPreparer preparer;
    const application::CoverArtResult cover = preparer.prepare(encoded(photo(2400, 2400), "png"));
    check(cover.cover.has_value(), "tags: a cover to embed (" + cover.error + ")");

    application::RecordingTags tags;
    tags.artist = "S\xC3\xA9" "bastien K\xC3\xBC" "gler & \xE5\x9D\x82\xE6\x9C\xAC";      // Sébastien Kügler & 坂本
    tags.title = "\xD0\x9D\xD0\xBE\xD1\x87\xD1\x8C \xD0\xB2 \xD0\xBA\xD0\xBB\xD1\x83\xD0\xB1\xD0\xB5";  // Ночь в клубе
    tags.album = "\xC5\x8Csaka Session \xF0\x9F\x8E\xA7";                           // Ōsaka Session and a headphone emoji
    tags.genre = "Drum & Bass";
    tags.year = 2026;
    if (cover.cover) {
        tags.cover = *cover.cover;
    }
    TagLibRecordingTagWriter writer;
    const auto written = writer.write(flac, tags);
    check(written.written && written.error.empty(), "tags: written (" + written.error + ")");
    check(!fs::exists(pathFromUtf8(flac + RecordingTagTempSuffix)), "tags: no temporary copy left");
    {
        const fs::path path = pathFromUtf8(flac);
        TagLib::FLAC::File file(path.c_str(), false);
        TagLib::Ogg::XiphComment *comment = file.xiphComment(false);
        check(field(comment, "ARTIST") == tags.artist, "tags: ARTIST reads back: " + field(comment, "ARTIST"));
        check(field(comment, "TITLE") == tags.title, "tags: TITLE reads back: " + field(comment, "TITLE"));
        check(field(comment, "ALBUM") == tags.album, "tags: ALBUM reads back: " + field(comment, "ALBUM"));
        check(field(comment, "GENRE") == tags.genre, "tags: GENRE reads back: " + field(comment, "GENRE"));
        check(field(comment, "DATE") == "2026", "tags: DATE reads back: " + field(comment, "DATE"));
        const auto pictures = file.pictureList();
        check(pictures.size() == 1, "tags: one picture");
        if (pictures.size() == 1 && cover.cover) {
            const TagLib::FLAC::Picture *picture = pictures.front();
            check(picture->type() == TagLib::FLAC::Picture::FrontCover, "tags: the picture is the front cover");
            check(picture->mimeType() == "image/jpeg", "tags: the picture is a JPEG");
            check(picture->width() == 1200 && picture->height() == 1200, "tags: the picture says 1200 x 1200");
            const TagLib::ByteVector data = picture->data();
            check(std::string(data.data(), data.size()) == cover.cover->jpeg, "tags: the picture's bytes are the cover's");
        }
    }
    const Audio tagged = audioOf(flac);
    check(tagged.streamInfo == untagged.streamInfo, "tags: STREAMINFO (and its MD5) is as it was");
    check(tagged.frames == untagged.frames, "tags: the audio frames are as they were, byte for byte");
    const std::string mismatch = recording_test::compareWithSignal(recording_test::decodeAll(flac), spec);
    check(mismatch.empty(), "tags: the tagged FLAC decodes to the signal: " + mismatch);

    // Again, with less: what is left empty goes, the cover too.
    application::RecordingTags fewer;
    fewer.title = "Second take";
    const auto again = writer.write(flac, fewer);
    check(again.written, "rewrite: written (" + again.error + ")");
    {
        const fs::path path = pathFromUtf8(flac);
        TagLib::FLAC::File file(path.c_str(), false);
        TagLib::Ogg::XiphComment *comment = file.xiphComment(false);
        check(field(comment, "TITLE") == "Second take", "rewrite: TITLE replaced: " + field(comment, "TITLE"));
        for (const char *gone : {"ARTIST", "ALBUM", "GENRE", "DATE"}) {
            check(field(comment, gone) == "<absent>", std::string("rewrite: ") + gone + " is gone");
        }
        check(file.pictureList().isEmpty(), "rewrite: the cover is gone");
    }
    const Audio rewritten = audioOf(flac);
    check(rewritten.streamInfo == untagged.streamInfo && rewritten.frames == untagged.frames,
          "rewrite: the audio is as it was");

    // A folder where the copy would go: a failure, the file as it was.
    {
        const std::string before = readAll(flac);
        QDir().mkpath(QString::fromStdString(flac + RecordingTagTempSuffix + "/inside"));
        const auto blocked = writer.write(flac, tags);
        check(!blocked.written && !blocked.error.empty(), "blocked copy: a failure (" + blocked.error + ")");
        check(readAll(flac) == before, "blocked copy: the file is as it was, byte for byte");
        check(fs::is_directory(pathFromUtf8(flac + RecordingTagTempSuffix + "/inside")),
              "blocked copy: the folder in the way is left alone");
    }
}

void notFlac(const std::string &dir)
{
    TagLibRecordingTagWriter writer;
    const std::string path = dir + "/not-audio.flac";
    std::ofstream(pathFromUtf8(path)) << "fLaX this is not a FLAC file at all";
    application::RecordingTags tags;
    tags.title = "x";
    const auto result = writer.write(path, tags);
    check(!result.written && result.error.find("not a FLAC") != std::string::npos,
          "not a FLAC: a failure that says so (" + result.error + ")");
    check(readAll(path) == "fLaX this is not a FLAC file at all", "not a FLAC: left as it was");
    check(!fs::exists(pathFromUtf8(path + RecordingTagTempSuffix)), "not a FLAC: no temporary copy");

    const auto missing = writer.write(dir + "/missing.flac", tags);
    check(!missing.written && !missing.error.empty(), "missing: a failure");
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: recording_tag_writer_test SCRATCH_DIR\n";
        return 2;
    }
    if (!QtFlacRecordingEncoder::encodingAvailable()) {
        std::cerr << "FAILED: no FLAC encoding and decoding backend\n";
        return 1;
    }
    if (!QImageWriter::supportedImageFormats().contains("jpeg")) {
        std::cerr << "FAILED: no JPEG image writer\n";
        return 1;
    }
    const QString scratchRoot = QCoreApplication::arguments().at(1);
    QDir().mkpath(scratchRoot);
    QTemporaryDir scratch(scratchRoot + "/recording-tags-XXXXXX");
    if (!scratch.isValid()) {
        std::cerr << "FAILED: no scratch directory\n";
        return 1;
    }
    const std::string dir = QDir(scratch.path()).absolutePath().toStdString();
    covers();
    tags(dir);
    notFlac(dir);
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "recording_tag_writer_test: all checks passed\n";
    return 0;
}
