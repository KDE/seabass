// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/taglib_recording_tag_writer.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <functional>
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
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/no_replace_rename.hpp"
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

// What the copy holds, read back with a fresh handle, against what was
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

// A FLAC that is not under its name, while a temporary copy of the tag
// writer is: what a power cut leaves on FAT and exFAT, where replacing the
// file by rename is a delete and then a rename on the disk. That copy may
// be the only one, so it is never deleted: when it is the only one and a
// FLAC, it is put back under the file's name; otherwise it is left alone.
// Either way the answer is a failure that says what was found.
//
// What can be proven of a copy that is put back: that it is a FLAC whose
// STREAMINFO block reads. Not whether it is the tagged copy that was
// checked (the rename that was cut short) or one whose write was cut short
// (and the file then removed by someone else): its audio and tags are not
// checked again here.
application::RecordingTagWriteResult recoverInterruptedWrite(const fs::path &target)
{
    application::RecordingTagWriteResult result;
    const std::string name = pathToUtf8(target.filename());
    const std::vector<fs::path> copies = tempsBeside(target.parent_path(), name, RecordingTagTempSuffix);
    if (copies.empty()) {
        result.error = "the tags of " + name + " were not written: it is not there";
        return result;
    }
    if (copies.size() > 1) {
        result.error = "the tags of " + name + " were not written: it is not there, and " + std::to_string(copies.size())
            + " temporary copies of it are; they were left as they are";
        return result;
    }
    const fs::path &copy = copies.front();
    const std::string copyName = pathToUtf8(copy.filename());
    if (!readFlacStreamInfo(copy)) {
        result.error = "the tags of " + name + " were not written: it is not there, and its temporary copy " + copyName
            + " is not a whole FLAC file; it was left as it is";
        return result;
    }
    const NoReplaceRename back = renameNoReplace(copy, target);
    if (back.status != NoReplaceRename::Status::Renamed) {
        result.error = "the tags of " + name + " were not written: it is not there, and its temporary copy " + copyName
            + " could not be put back under its name (" + (back.error.empty() ? "a file appeared there" : back.error)
            + "); it was left as it is";
        return result;
    }
    try {
        fsyncDirectoryContaining(pathToUtf8(target));
    } catch (...) {
    }
    result.error = "the tags of " + name + " were not written: an earlier write of them was cut short and left the "
        "file only as its temporary copy " + copyName + ", which was put back under its name. Its audio and tags "
        "were not checked again";
    return result;
}

application::RecordingTagWriteResult write(const fs::path &target, const application::RecordingTags &tags,
                                           const std::function<void(const std::string &)> &beforePlace)
{
    application::RecordingTagWriteResult result;
    const std::string name = pathToUtf8(target.filename());
    // A name of this write's own: two writes never take each other's copy.
    const fs::path temp = uniqueTempBeside(target, RecordingTagTempSuffix);
    bool tempMade = false;
    const auto fail = [&](const std::string &why) {
        if (tempMade && !sameFile(temp, target)) {
            std::error_code ignored;
            if (fs::symlink_status(longPathSafe(temp), ignored).type() == fs::file_type::regular) {
                fs::remove(longPathSafe(temp), ignored);
            }
        }
        result = {};
        result.error = "the tags of " + name + " were not written: " + why;
        return result;
    };

    try {
        const std::optional<bool> there = somethingAt(target);
        if (!there) {
            return fail("it could not be looked at");
        }
        if (!*there) {
            return recoverInterruptedWrite(target);
        }
        const std::optional<FlacStreamInfo> before = readFlacStreamInfo(target);
        if (!before) {
            return fail("it is not a FLAC file");
        }
        if (somethingAt(temp) != std::optional<bool>(false)) {
            return fail("something is in the way of its temporary copy " + pathToUtf8(temp.filename()));
        }
        tempMade = true;
        std::error_code ec;
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
        const std::optional<std::uint64_t> checkedSize = fileSizeOf(temp);
        const std::optional<FlacStreamInfo> after = readFlacStreamInfo(temp);
        if (!checkedSize || !after || after->raw != before->raw || audioHash(temp, after->audioOffset) != audioBefore) {
            return fail("the audio would not have stayed as it was");
        }
        if (beforePlace) {
            beforePlace(pathToUtf8(temp));
        }
        // Opened without being created: a copy that went away after the
        // check is a failure, never an empty file in the FLAC's place.
        std::uint64_t flushedSize = 0;
        try {
            flushedSize = flushExistingFile(temp);
        } catch (const std::exception &e) {
            return fail(std::string("the tagged copy could not be flushed: ") + e.what());
        }
        // The copy that was flushed is the one that was checked.
        const std::optional<FlacStreamInfo> flushed = readFlacStreamInfo(temp);
        if (flushedSize != *checkedSize || !flushed || flushed->raw != before->raw
            || audioHash(temp, flushed->audioOffset) != audioBefore) {
            return fail("the tagged copy changed after it was checked");
        }
        // Replaces the file: that is the point. On FAT and exFAT this is a
        // delete and a rename on the disk; a power cut between them leaves
        // only the copy, which recoverInterruptedWrite() puts back.
        fs::rename(longPathSafe(temp), longPathSafe(target), ec);
        if (ec) {
            return fail("the tagged copy could not take the file's place: " + ec.message());
        }
        tempMade = false;
        try {
            fsyncDirectoryContaining(pathToUtf8(target));
        } catch (...) {
        }
        if (fileSizeOf(target) != checkedSize) {
            result.error = "the tags of " + name + " were written, but the file under its name does not have the "
                "size of the checked copy afterwards";
            return result;
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
        return audio::write(pathFromUtf8(flacPath), tags, m_beforePlace);
    } catch (...) {
        application::RecordingTagWriteResult result;
        result.error = "the tags were not written";
        return result;
    }
}

}  // namespace seabass::infrastructure::audio
