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
#include "infrastructure/durable_file_write.hpp"
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
    // fs::path's c_str() is the wchar_t* TagLib takes on Windows.
    TagLib::FLAC::File file(path.c_str(), false);
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

// What the copy holds, read back with a fresh handle, against what was
// asked for. Empty when they agree.
std::string compareTags(const fs::path &path, const application::RecordingTags &tags)
{
    TagLib::FLAC::File file(path.c_str(), false);
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

application::RecordingTagWriteResult write(const fs::path &target, const application::RecordingTags &tags)
{
    application::RecordingTagWriteResult result;
    const std::string name = pathToUtf8(target.filename());
    fs::path temp = target;
    temp += RecordingTagTempSuffix;
    bool tempMade = false;
    const auto fail = [&](const std::string &why) {
        if (tempMade) {
            std::error_code ignored;
            fs::remove(longPathSafe(temp), ignored);
        }
        result = {};
        result.error = "the tags of " + name + " were not written: " + why;
        return result;
    };

    try {
        const std::optional<FlacStreamInfo> before = readFlacStreamInfo(target);
        if (!before) {
            return fail("it is not a FLAC file");
        }
        std::error_code ec;
        const fs::file_status leftover = fs::symlink_status(longPathSafe(temp), ec);
        if (leftover.type() == fs::file_type::regular || leftover.type() == fs::file_type::symlink) {
            fs::remove(longPathSafe(temp), ec);
        } else if (leftover.type() != fs::file_type::not_found) {
            return fail("something is in the way of its temporary copy " + pathToUtf8(temp.filename()));
        }
        tempMade = true;
        if (!fs::copy_file(longPathSafe(target), longPathSafe(temp), fs::copy_options::none, ec) || ec) {
            return fail("it could not be copied beside itself: " + ec.message());
        }
        const hashing::Sha256Digest audioBefore = audioHash(target, before->audioOffset);

        if (!writeTags(temp, tags)) {
            return fail("TagLib could not save them");
        }
        const std::string mismatch = compareTags(temp, tags);
        if (!mismatch.empty()) {
            return fail(mismatch);
        }
        const std::optional<FlacStreamInfo> after = readFlacStreamInfo(temp);
        if (!after || after->raw != before->raw || audioHash(temp, after->audioOffset) != audioBefore) {
            return fail("the audio would not have stayed as it was");
        }
        {
            PosixArchiveFile written(longPathSafe(temp), PosixArchiveFile::OpenMode::ReadWrite);
            written.barrier();
        }
        fs::rename(longPathSafe(temp), longPathSafe(target), ec);
        if (ec) {
            return fail("the tagged copy could not take the file's place: " + ec.message());
        }
        tempMade = false;
        try {
            fsyncDirectoryContaining(pathToUtf8(target));
        } catch (...) {
        }
        result.written = true;
        return result;
    } catch (const std::exception &e) {
        return fail(e.what());
    }
}

}  // namespace

application::RecordingTagWriteResult TagLibRecordingTagWriter::write(const std::string &flacPath,
                                                                     const application::RecordingTags &tags)
{
    try {
        return audio::write(pathFromUtf8(flacPath), tags);
    } catch (...) {
        application::RecordingTagWriteResult result;
        result.error = "the tags were not written";
        return result;
    }
}

}  // namespace seabass::infrastructure::audio
