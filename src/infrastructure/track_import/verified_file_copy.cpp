// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/track_import/verified_file_copy.hpp"

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"

namespace seabass::infrastructure::track_import
{

namespace
{

namespace fs = std::filesystem;
using stick_backup::PosixArchiveFile;

// The SHA-256 of a whole file, read in pieces. nullopt on a cancel.
std::optional<hashing::Sha256Digest> hashOf(const PosixArchiveFile &file, std::vector<std::byte> &buffer,
                                            const application::CancellationToken &cancel)
{
    hashing::Sha256 hasher;
    for (std::uint64_t offset = 0; offset < file.size();) {
        if (cancel.cancelled()) {
            return std::nullopt;
        }
        const std::size_t piece = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), file.size() - offset));
        file.readAt(offset, std::span<std::byte>(buffer.data(), piece));
        hasher.update(std::span<const std::byte>(buffer.data(), piece));
        offset += piece;
    }
    return hasher.finish();
}

// Whether something is under `path`, a dangling link included. nullopt
// when the filesystem would not say (no permission, a stick being
// pulled): "could not ask" is not "nothing there".
std::optional<bool> somethingAt(const fs::path &path)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(longPathSafe(path), ec);
    if (status.type() == fs::file_type::not_found) {
        return false;
    }
    if (ec || status.type() == fs::file_type::none) {
        return std::nullopt;
    }
    return true;
}

// The folders create_directories() would have to make for `folder`,
// deepest first: what a copy that then fails takes away again.
std::vector<fs::path> missingFolders(const fs::path &folder)
{
    std::vector<fs::path> missing;
    std::error_code ec;
    for (fs::path at = folder; !at.empty() && at != at.root_path(); at = at.parent_path()) {
        // Not exists(), which follows a link: a link that points nowhere
        // is something this call did not make and must not take away.
        const fs::file_status status = fs::symlink_status(longPathSafe(at), ec);
        if (status.type() != fs::file_type::not_found) {
            break;
        }
        missing.push_back(at);
    }
    return missing;
}

// `text` with each of `paths` in it cut to the file's name.
std::string namesOnly(std::string text, std::initializer_list<fs::path> paths)
{
    for (const fs::path &path : paths) {
        const std::string spelled = pathToUtf8(path);
        const std::string name = pathToUtf8(path.filename());
        if (spelled.empty() || spelled == name) {
            continue;
        }
        for (size_t at = text.find(spelled); at != std::string::npos; at = text.find(spelled, at + name.size())) {
            text.replace(at, spelled.size(), name);
        }
    }
    return text;
}

FileCopyResult copy(const fs::path &source, const fs::path &destination, const FileCopyOptions &options)
{
    FileCopyResult result;
    const std::string name = pathToUtf8(destination.filename());
    // The source by its name only, as every text here says a file: the
    // errors go into the stick's log, and the computer's folders do not.
    const std::string sourceName = pathToUtf8(source.filename());
    std::error_code ec;

    if (!fs::is_regular_file(longPathSafe(source), ec)) {
        result.error = sourceName + " is not a file that can be read";
        return result;
    }
    fs::path temp = destination;
    temp += ImportTempSuffix;

    const auto refusedPlace = [&](const fs::path &place) {
        const std::optional<bool> there = somethingAt(place);
        if (!there) {
            result.error = "could not look at the place for " + name + " on the stick; not copied";
        } else if (*there && place == destination) {
            result.error = name + " is on the stick already; not replaced";
        }
        return !there || (*there && place == destination);
    };
    if (refusedPlace(destination)) {
        return result;
    }
    // A source offered under the temporary name would be emptied by the
    // copy of itself.
    if (fs::equivalent(longPathSafe(source), longPathSafe(temp), ec)) {
        result.error = sourceName + " is the leftover of an interrupted import; not copied";
        return result;
    }
    ec.clear();

    const std::vector<fs::path> made = missingFolders(destination.parent_path());
    // Takes away what this call put there: the temporary file, and the
    // folders it made, while they are empty.
    const auto discard = [&]() {
        std::error_code ignored;
        fs::remove(longPathSafe(temp), ignored);
        for (const fs::path &folder : made) {
            fs::remove(longPathSafe(folder), ignored);  // refuses a folder with something in it
        }
    };
    const auto fail = [&](std::string why) {
        discard();
        result = {};
        result.error = std::move(why);
        return result;
    };

    try {
        fs::create_directories(longPathSafe(destination.parent_path()), ec);
        if (ec) {
            return fail("could not create the folder for " + name + ": " + ec.message());
        }
        // A leftover goes rather than being opened: it may be a link.
        fs::remove(longPathSafe(temp), ec);
        ec.clear();

        // The source's time before the first byte is read, to know
        // afterwards that nothing wrote to it meanwhile.
        const fs::file_time_type modified = fs::last_write_time(longPathSafe(source), ec);
        if (ec) {
            return fail("could not read the modification time of " + sourceName);
        }

        std::vector<std::byte> buffer(std::max<std::size_t>(options.chunkSize, 4096));
        const PosixArchiveFile in(longPathSafe(source), PosixArchiveFile::OpenMode::ReadOnly);
        const std::uint64_t size = in.size();
        hashing::Sha256 hasher;
        bool cancelled = false;
        {
            PosixArchiveFile out(longPathSafe(temp), PosixArchiveFile::OpenMode::ReadWrite);
            // The temporary file starts empty whatever the remove above
            // did: ReadWrite opens without truncating, and bytes appended
            // after a leftover's would be a file with the wrong start
            // (restore_stick_backup.cpp's writeEntry, for the same file
            // class, says how that went unnoticed there).
            out.truncate(0);
            for (std::uint64_t offset = 0; offset < size;) {
                if (options.cancel.cancelled()) {
                    cancelled = true;
                    break;
                }
                const std::size_t piece = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
                in.readAt(offset, std::span<std::byte>(buffer.data(), piece));
                out.append(std::span<const std::byte>(buffer.data(), piece));
                hasher.update(std::span<const std::byte>(buffer.data(), piece));
                offset += piece;
                if (options.progress) {
                    options.progress(offset);
                }
            }
            if (!cancelled) {
                out.barrier();
            }
        }
        if (cancelled) {
            discard();
            result.cancelled = true;
            return result;
        }
        const hashing::Sha256Digest read = hasher.finish();

        // The source as it is now: one that grew, shrank or was written
        // to while it was read (a tagger rewrites a tag in place, at the
        // same size) is not one file's bytes.
        const std::uintmax_t sizeNow = fs::file_size(longPathSafe(source), ec);
        const bool sizeKept = !ec && sizeNow == size;
        const fs::file_time_type modifiedNow = fs::last_write_time(longPathSafe(source), ec);
        if (!sizeKept || ec || modifiedNow != modified) {
            return fail(sourceName + " changed while it was copied; not imported");
        }

        // What landed, read back through a handle of its own.
        {
            const PosixArchiveFile landed(longPathSafe(temp), PosixArchiveFile::OpenMode::ReadOnly);
            if (landed.size() != size) {
                return fail(name + " did not arrive on the stick as it was read; not imported");
            }
            const auto found = hashOf(landed, buffer, options.cancel);
            if (!found) {
                cancelled = true;
            } else if (*found != read) {
                return fail(name + " did not arrive on the stick as it was read; not imported");
            }
        }
        if (cancelled) {
            discard();
            result.cancelled = true;
            return result;
        }

        fs::last_write_time(longPathSafe(temp), modified, ec);
        ec.clear();
        // Asked again: the copy of a large file takes minutes. Between
        // this and the rename nothing else writes the stick, which the
        // stick's write lock is for; the rename itself would replace.
        if (refusedPlace(destination)) {
            return fail(result.error);
        }
        fs::rename(longPathSafe(temp), longPathSafe(destination), ec);
        if (ec) {
            return fail("could not place " + name + ": " + ec.message());
        }
        // The file is in place from here, whatever the flush of its
        // folder's entry does.
        result.copied = true;
        result.bytes = size;
        result.sha256 = read;
        try {
            fsyncDirectoryContaining(pathToUtf8(destination));
        } catch (...) {
        }
        return result;
    } catch (const std::exception &e) {
        discard();
        result = {};
        result.error = "copy failed";  // short enough to need no memory
        // The reader's own text names the files in full: they are said
        // by name.
        result.error = "could not copy " + name + ": " + namesOnly(e.what(), {longPathSafe(source), longPathSafe(temp), longPathSafe(destination),
                                                                         source, temp, destination});
        return result;
    } catch (...) {
        // Not a std::exception: a progress callback's own.
        discard();
        result = {};
        result.error = "copy failed";
        return result;
    }
}

bool endsWithTempSuffix(const std::string &name)
{
    const std::string suffix = ImportTempSuffix;
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<fs::path> sweep(const fs::path &stickRoot, const fs::path &folder)
{
    std::vector<fs::path> removed;
    const fs::path root = stickRoot.lexically_normal();
    const fs::path relative = folder.lexically_normal().lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return removed;
    }
    // The root and every folder below it on the way: a folder, never a
    // link, so nothing outside the stick is reached.
    fs::path at = root;
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(longPathSafe(at), ec)) || ec) {
        return removed;
    }
    for (const fs::path &segment : relative) {
        if (segment.empty() || segment == ".") {
            continue;
        }
        if (segment == "..") {
            return removed;
        }
        at /= segment;
        if (!fs::is_directory(fs::symlink_status(longPathSafe(at), ec)) || ec) {
            return removed;
        }
    }
    std::vector<fs::path> leftovers;
    {
        DirectoryReader reader(at, ec);
        if (ec) {
            return removed;
        }
        fs::path child;
        while (reader.next(child, ec)) {
            std::error_code statEc;
            const fs::file_status status = fs::symlink_status(longPathSafe(child), statEc);
            if (!statEc && fs::is_regular_file(status) && endsWithTempSuffix(pathToUtf8(child.filename()))) {
                leftovers.push_back(child);
            }
        }
    }
    for (const fs::path &leftover : leftovers) {
        std::error_code removeEc;
        if (fs::remove(longPathSafe(leftover), removeEc) && !removeEc) {
            removed.push_back(leftover);
        }
    }
    return removed;
}

}  // namespace

std::vector<fs::path> sweepImportLeftovers(const fs::path &stickRoot, const fs::path &folder)
{
    try {
        return sweep(stickRoot, folder);
    } catch (...) {
        return {};
    }
}

FileCopyResult copyFileVerified(const fs::path &source, const fs::path &destination, const FileCopyOptions &options)
{
    try {
        return copy(source, destination, options);
    } catch (...) {
        // A path that cannot be spelled, or no memory for the message.
        FileCopyResult result;
        result.error = "copy failed";  // fits a string's own buffer: cannot throw
        return result;
    }
}

}  // namespace seabass::infrastructure::track_import
