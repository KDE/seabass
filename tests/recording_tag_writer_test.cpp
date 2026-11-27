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
//   - the tags go into the encoder's proven file under its temporary name
//     (no ".flac" at the end), in place: no other file appears in the
//     folder, the file keeps its identity (inode) while it grows past its
//     padding for a large cover, and the answer describes it as it is
//     now, which placeVerified() then gives the FLAC's name;
//   - the audio is untouched: the same STREAMINFO (with its MD5), the
//     same audio frames byte for byte, and it decodes to the signal;
//   - a file that is not the one described (another size, another
//     STREAMINFO), a file that is not a FLAC, one that is not there: a
//     failure, the file left byte for byte as it was;
//   - the extra bytes the temporary name adds are the encoder's suffix's
//     (recordingTempNameExtraBytes());
//   - a large picture is decoded at the cover's size, absurd sizes are
//     refused before decoding, and an extreme shape keeps a side of at
//     least one pixel.
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

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/xiphcomment.h>

#include "application/ports/cover_art_preparer.hpp"
#include "infrastructure/audio/flac_stream_info.hpp"
#include "infrastructure/audio/qt_cover_art_preparer.hpp"
#include "infrastructure/audio/qt_flac_recording_encoder.hpp"
#include "infrastructure/audio/recording_file_steps.hpp"
#include "infrastructure/audio/taglib_recording_tag_writer.hpp"
#include "recording_test_support.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace seabass;
using infrastructure::audio::QtCoverArtPreparer;
using infrastructure::audio::QtFlacRecordingEncoder;
using infrastructure::audio::RecordingEncodeTempSuffix;
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

// The names in a folder, sorted.
std::vector<std::string> entries(const std::string &folder)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(pathFromUtf8(folder), ec)) {
        names.push_back(pathToUtf8(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

#if !defined(_WIN32)
// The file's identity: the same inode is the same file, written in place.
ino_t inodeOf(const std::string &path)
{
    struct stat st{};
    return ::stat(pathToUtf8(pathFromUtf8(path)).c_str(), &st) == 0 ? st.st_ino : 0;
}
#endif

// The encoder's proven file for `flac`, left unplaced under its temporary
// name, as a keep job has it before the tags.
application::VerifiedFlac unplaced(QtFlacRecordingEncoder &encoder, const std::string &wav, const std::string &flac)
{
    application::RecordingEncodeOptions options;
    options.leaveUnplaced = true;
    const auto made = encoder.encode(wav, flac, options);
    check(made.encoded && !made.unplaced.path.empty(), "the FLAC to tag is made, unplaced (" + made.error + ")");
    return made.unplaced;
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
    // An extreme shape: the short side stays at least one pixel.
    {
        const application::CoverArtResult result = preparer.prepare(encoded(photo(6000, 2), "png"));
        check(result.cover && result.cover->width == 1200 && result.cover->height == 1,
              "a long thin picture: 1200 x 1 (" + result.error + ")");
    }
    // A header that claims an absurd size: refused before anything is
    // decoded. A PNG's IHDR says 100000 x 100000; no pixel data follows.
    {
        const QByteArray png = QByteArray::fromHex("89504e470d0a1a0a0000000d49484452000186a0000186a0080200000027309c9f"
                                                   "000000004944415435af061e");
        const application::CoverArtResult result = preparer.prepare(std::string(png.constData(), static_cast<std::size_t>(png.size())));
        check(!result.cover && result.error.find("100000 x 100000") != std::string::npos,
              "an absurd size: refused before decoding (" + result.error + ")");
    }
    // A large JPEG is decoded at the cover's size: the result is the same
    // picture, scaled.
    {
        const application::CoverArtResult result = preparer.prepare(encoded(photo(4800, 3600), "jpeg"));
        check(result.cover && result.cover->width == 1200 && result.cover->height == 900,
              "a large JPEG: 1200 x 900 (" + result.error + ")");
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
    const std::string folder = dir + "/tags";
    fs::create_directories(pathFromUtf8(folder));
    const std::string wav = dir + "/tagged.wav";
    const std::string flac = folder + "/Unicode-\xC3\xA9\xC3\x9F set.flac";
    recording_test::writeWav(wav, spec);
    QtFlacRecordingEncoder encoder;
    const application::VerifiedFlac proven = unplaced(encoder, wav, flac);
    if (proven.path.empty()) {
        return;
    }
    const std::string suffix = ".seabass-encode-tmp";  // as written, not the constant
    check(proven.path.size() > suffix.size() && proven.path.compare(proven.path.size() - suffix.size(), suffix.size(), suffix) == 0,
          "tags: the file tagged is the encoder's temporary file, its name not ending in .flac: " + proven.path);
    const Audio untagged = audioOf(proven.path);
    check(!untagged.frames.empty(), "tags: the untagged FLAC's audio is found");
    const std::vector<std::string> before = entries(folder);
#if !defined(_WIN32)
    const ino_t inode = inodeOf(proven.path);
#endif

    QtCoverArtPreparer preparer;
    const application::CoverArtResult cover = preparer.prepare(encoded(photo(2400, 2400), "png"));
    check(cover.cover.has_value(), "tags: a cover to embed (" + cover.error + ")");

    application::RecordingTags tags;
    tags.artist = "S\xC3\xA9" "verine M\xC3\xBC" "ller & \xE5\x9D\x82\xE6\x9C\xAC";      // Séverine Müller & 坂本
    tags.title = "\xD0\x9D\xD0\xBE\xD1\x87\xD1\x8C \xD0\xB2 \xD0\xBA\xD0\xBB\xD1\x83\xD0\xB1\xD0\xB5";  // Ночь в клубе
    tags.album = "\xC5\x8Csaka Session \xF0\x9F\x8E\xA7";                           // Ōsaka Session and a headphone emoji
    tags.genre = "Drum & Bass";
    tags.year = 2026;
    if (cover.cover) {
        tags.cover = *cover.cover;
    }
    TagLibRecordingTagWriter writer;
    const auto written = writer.tagInPlace(proven, tags);
    check(written.written && written.error.empty(), "tags: written (" + written.error + ")");
    check(entries(folder) == before, "tags: no other file appeared in the folder, no copy and no temporary file");
#if !defined(_WIN32)
    check(inode != 0 && inodeOf(proven.path) == inode, "tags: written in place, the same file (inode) as before");
#endif
    const std::uint64_t taggedSize = fs::file_size(pathFromUtf8(proven.path));
    check(cover.cover && taggedSize > proven.size && taggedSize - proven.size > cover.cover->jpeg.size() / 2,
          "tags: the file grew past its padding for the cover (" + std::to_string(proven.size) + " to "
              + std::to_string(taggedSize) + " bytes)");
    check(written.tagged.path == proven.path && written.tagged.size == taggedSize
              && written.tagged.streamInfo == proven.streamInfo,
          "tags: the answer describes the file as it is now: its size, its STREAMINFO unchanged");
    {
        const fs::path path = pathFromUtf8(proven.path);
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
    const Audio tagged = audioOf(proven.path);
    check(tagged.streamInfo == untagged.streamInfo, "tags: STREAMINFO (and its MD5) is as it was");
    check(tagged.frames == untagged.frames, "tags: the audio frames are as they were, byte for byte");
    const std::string mismatch = recording_test::compareWithSignal(recording_test::decodeAll(proven.path), spec);
    check(mismatch.empty(), "tags: the tagged FLAC decodes to the signal: " + mismatch);

    // Again, with less: what is left empty goes, the cover too.
    application::RecordingTags fewer;
    fewer.title = "Second take";
    const auto again = writer.tagInPlace(written.tagged, fewer);
    check(again.written, "rewrite: written (" + again.error + ")");
    {
        const fs::path path = pathFromUtf8(proven.path);
        TagLib::FLAC::File file(path.c_str(), false);
        TagLib::Ogg::XiphComment *comment = file.xiphComment(false);
        check(field(comment, "TITLE") == "Second take", "rewrite: TITLE replaced: " + field(comment, "TITLE"));
        for (const char *gone : {"ARTIST", "ALBUM", "GENRE", "DATE"}) {
            check(field(comment, gone) == "<absent>", std::string("rewrite: ") + gone + " is gone");
        }
        check(file.pictureList().isEmpty(), "rewrite: the cover is gone");
    }
    const Audio rewritten = audioOf(proven.path);
    check(rewritten.streamInfo == untagged.streamInfo && rewritten.frames == untagged.frames,
          "rewrite: the audio is as it was");
    check(again.tagged.size == fs::file_size(pathFromUtf8(proven.path)), "rewrite: the answer has the size now");

    // The description handed on is what the placing checks: the tagged
    // file gets the FLAC's name, and is the signal with its tags.
    const auto placed = encoder.placeVerified(again.tagged, flac);
    check(placed.placed, "placed after the tags (" + placed.error + ")");
    check(entries(folder) == std::vector<std::string>{pathToUtf8(pathFromUtf8(flac).filename())},
          "placed: the FLAC alone in the folder, no temporary file left");
    check(recording_test::compareWithSignal(recording_test::decodeAll(flac), spec).empty(),
          "placed: the FLAC decodes to the signal");
    // Placing with the description from before the tags is refused: the
    // file is not that one any more.
    const application::VerifiedFlac second = unplaced(encoder, wav, folder + "/Stale.flac");
    const auto taggedSecond = writer.tagInPlace(second, tags);
    const auto stale = encoder.placeVerified(second, folder + "/Stale.flac");
    check(taggedSecond.written && !stale.placed && stale.error.find("changed after it was checked") != std::string::npos,
          "placing with the description from before the tags: refused (" + stale.error + ")");
    check(!fs::exists(pathFromUtf8(folder + "/Stale.flac")) && fs::exists(pathFromUtf8(second.path)),
          "and nothing has the FLAC's name, the file stays under its own");
}

// A file that is not the one described is not written to.
void notTheFileChecked(const std::string &dir)
{
    const recording_test::WavSpec spec{44100, 2, 16, 44100 * 2};
    const std::string wav = dir + "/described.wav";
    recording_test::writeWav(wav, spec);
    QtFlacRecordingEncoder encoder;
    TagLibRecordingTagWriter writer;
    application::RecordingTags tags;
    tags.title = "Described";
    const application::VerifiedFlac proven = unplaced(encoder, wav, dir + "/described.flac");
    if (proven.path.empty()) {
        return;
    }
    const std::string bytes = readAll(proven.path);
    {
        application::VerifiedFlac other = proven;
        other.size += 1;
        const auto result = writer.tagInPlace(other, tags);
        check(!result.written && result.error.find("not the file that was checked") != std::string::npos,
              "another size than described: refused (" + result.error + ")");
        check(readAll(proven.path) == bytes, "another size than described: the file is as it was, byte for byte");
    }
    {
        application::VerifiedFlac other = proven;
        other.streamInfo[33] ^= 0x01;  // the MD5's last byte
        const auto result = writer.tagInPlace(other, tags);
        check(!result.written && result.error.find("not the file that was checked") != std::string::npos,
              "another STREAMINFO than described: refused (" + result.error + ")");
        check(readAll(proven.path) == bytes, "another STREAMINFO than described: the file is as it was");
    }
    check(infrastructure::audio::recordingTempNameExtraBytes()
              == 1 + 16 + std::string_view(RecordingEncodeTempSuffix).size(),
          "the name budget's extra bytes are the encoder's temporary name's: "
              + std::to_string(infrastructure::audio::recordingTempNameExtraBytes()));
    fs::remove(pathFromUtf8(proven.path));
}

void notFlac(const std::string &dir)
{
    TagLibRecordingTagWriter writer;
    const std::string path = dir + "/not-audio.flac";
    std::ofstream(pathFromUtf8(path)) << "fLaX this is not a FLAC file at all";
    application::RecordingTags tags;
    tags.title = "x";
    application::VerifiedFlac file;
    file.path = path;
    file.size = fs::file_size(pathFromUtf8(path));
    const auto result = writer.tagInPlace(file, tags);
    check(!result.written && result.error.find("not a FLAC") != std::string::npos,
          "not a FLAC: a failure that says so (" + result.error + ")");
    check(readAll(path) == "fLaX this is not a FLAC file at all", "not a FLAC: left as it was");

    application::VerifiedFlac missing;
    missing.path = dir + "/missing.flac";
    const auto gone = writer.tagInPlace(missing, tags);
    check(!gone.written && gone.error.find("it is not there") != std::string::npos,
          "missing: a failure that says it is not there (" + gone.error + ")");
    check(!fs::exists(pathFromUtf8(missing.path)), "missing: nothing made");
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
    notTheFileChecked(dir);
    notFlac(dir);
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "recording_tag_writer_test: all checks passed\n";
    return 0;
}
