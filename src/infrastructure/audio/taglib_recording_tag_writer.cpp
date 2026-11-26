// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/taglib_recording_tag_writer.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/tbytevector.h>
#include <taglib/tstring.h>
#include <taglib/xiphcomment.h>

#include "infrastructure/audio/flac_stream_info.hpp"
#include "infrastructure/audio/recording_file_steps.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

namespace fs = std::filesystem;
using stick_backup::PosixArchiveFile;

// The fields written, as the Vorbis comment names them.
struct Field
{
    const char *name;
    std::string value;
};

std::vector<Field> fieldsOf(const application::RecordingTags &tags)
{
    return {
        {"ARTIST", tags.artist},
        {"TITLE", tags.title},
        {"ALBUM", tags.album},
        {"GENRE", tags.genre},
        {"DATE", tags.year > 0 ? std::to_string(tags.year) : std::string()},
    };
}

TagLib::String utf8(const std::string &text)
{
    return TagLib::String(text, TagLib::String::UTF8);
}

// The SHA-256 of a file's audio frames: from `offset` to its end.
hashing::Sha256Digest audioHash(const fs::path &path, std::uint64_t offset)
{
    const PosixArchiveFile file(longPathSafe(path), PosixArchiveFile::OpenMode::ReadOnly);
    hashing::Sha256 hasher;
    std::vector<std::byte> buffer(std::size_t{1} << 20);
    for (std::uint64_t at = offset; at < file.size();) {
        const auto piece = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), file.size() - at));
        file.readAt(at, std::span<std::byte>(buffer.data(), piece));
        hasher.update(std::span<const std::byte>(buffer.data(), piece));
        at += piece;
    }
    return hasher.finish();
}

bool writeTags(const fs::path &path, const application::RecordingTags &tags)
{
    // fs::path's c_str() is the wchar_t* TagLib's FileName takes on
    // Windows, so a name outside ASCII arrives whole; longPathSafe() gives
    // a path past MAX_PATH its prefix.
    const fs::path name = longPathSafe(path);
    TagLib::FLAC::File file(name.c_str(), false);
    if (!file.isValid()) {
        return false;
    }
    TagLib::Ogg::XiphComment *comment = file.xiphComment(true);
    if (comment == nullptr) {
        return false;
    }
    for (const Field &field : fieldsOf(tags)) {
        comment->removeFields(field.name);
        if (!field.value.empty()) {
            comment->addField(field.name, utf8(field.value), true);
        }
    }
    file.removePictures();
    if (!tags.cover.jpeg.empty()) {
        auto *picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/jpeg");
        picture->setWidth(tags.cover.width);
        picture->setHeight(tags.cover.height);
        picture->setColorDepth(24);
        picture->setNumColors(0);
        picture->setData(TagLib::ByteVector(tags.cover.jpeg.data(), static_cast<unsigned int>(tags.cover.jpeg.size())));
        file.addPicture(picture);  // the file owns it from here
    }
    return file.save();
}

// What the file holds, read back with a fresh handle, against what was
// asked for. Empty when they agree.
std::string compareTags(const fs::path &path, const application::RecordingTags &tags)
{
    const fs::path name = longPathSafe(path);
    TagLib::FLAC::File file(name.c_str(), false);
    if (!file.isValid()) {
        return "the tagged file could not be read back";
    }
    TagLib::Ogg::XiphComment *comment = file.xiphComment(false);
    for (const Field &field : fieldsOf(tags)) {
        const bool has = comment != nullptr && comment->contains(field.name);
        if (field.value.empty()) {
            if (has) {
                return std::string(field.name) + " is in the file although it was left empty";
            }
            continue;
        }
        if (!has) {
            return std::string(field.name) + " did not arrive";
        }
        const TagLib::StringList values = comment->fieldListMap()[field.name];
        if (values.size() != 1 || values.front().to8Bit(true) != field.value) {
            return std::string(field.name) + " reads back as another value";
        }
    }
    const TagLib::List<TagLib::FLAC::Picture *> pictures = file.pictureList();
    if (tags.cover.jpeg.empty()) {
        if (!pictures.isEmpty()) {
            return "a picture is in the file although none was given";
        }
        return {};
    }
    if (pictures.size() != 1) {
        return "the file holds " + std::to_string(pictures.size()) + " pictures, not the one cover";
    }
    const TagLib::FLAC::Picture *picture = pictures.front();
    const TagLib::ByteVector data = picture->data();
    if (picture->type() != TagLib::FLAC::Picture::FrontCover || picture->mimeType() != "image/jpeg"
        || picture->width() != tags.cover.width || picture->height() != tags.cover.height
        || data.size() != tags.cover.jpeg.size()
        || !std::equal(data.begin(), data.end(), tags.cover.jpeg.begin())) {
        return "the cover reads back as another picture";
    }
    return {};
}

application::RecordingTagWriteResult tagInPlace(const application::VerifiedFlac &verified,
                                                const application::RecordingTags &tags)
{
    application::RecordingTagWriteResult result;
    const fs::path path = pathFromUtf8(verified.path);
    const std::string name = pathToUtf8(path.filename());
    const auto fail = [&](const std::string &why) {
        result = {};
        result.error = "the tags of " + name + " were not written: " + why;
        return result;
    };

    try {
        std::error_code lookedAt;
        const std::optional<bool> there = somethingAt(path, &lookedAt);
        if (!there) {
            return fail("it could not be looked at (" + lookedAt.message() + ")");
        }
        if (!*there) {
            return fail("it is not there");
        }
        // The file the encoder proved, as it proved it: its size and its
        // STREAMINFO block. Only then is its audio hashed, to be compared
        // with the audio after the write.
        const std::optional<FlacStreamInfo> before = readFlacStreamInfo(path);
        if (!before) {
            return fail("it is not a FLAC file");
        }
        if (fileSizeOf(path) != std::optional<std::uint64_t>(verified.size) || before->raw != verified.streamInfo) {
            return fail("it is not the file that was checked");
        }
        const hashing::Sha256Digest audioBefore = audioHash(path, before->audioOffset);

        if (!writeTags(path, tags)) {
            return fail("TagLib could not save them");
        }
        const std::string mismatch = compareTags(path, tags);
        if (!mismatch.empty()) {
            return fail(mismatch);
        }
        const std::optional<std::uint64_t> taggedSize = fileSizeOf(path);
        const std::optional<FlacStreamInfo> after = readFlacStreamInfo(path);
        if (!taggedSize || !after || after->raw != before->raw || audioHash(path, after->audioOffset) != audioBefore) {
            return fail("the audio did not stay as it was");
        }
        result.written = true;
        result.tagged.path = verified.path;
        result.tagged.size = *taggedSize;
        result.tagged.streamInfo = after->raw;
        return result;
    } catch (const std::exception &e) {
        return fail(e.what());
    }
}

}  // namespace

application::RecordingTagWriteResult TagLibRecordingTagWriter::tagInPlace(const application::VerifiedFlac &file,
                                                                          const application::RecordingTags &tags)
{
    try {
        return audio::tagInPlace(file, tags);
    } catch (...) {
        application::RecordingTagWriteResult result;
        result.error = "the tags were not written";
        return result;
    }
}

}  // namespace seabass::infrastructure::audio
