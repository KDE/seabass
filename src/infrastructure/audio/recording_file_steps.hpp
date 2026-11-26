// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// What the recording encoder and the recording tag writer share about the
// files they write: the name of a temporary file, how two paths are
// compared, and the flush of a file that must already exist.
namespace seabass::infrastructure::audio
{

// A temporary name beside `target` that no other job uses:
// "<target's name>.<16 hex digits><suffix>". The suffix stays the last
// part of the name, so a scan that tidies leftovers recognises it.
std::filesystem::path uniqueTempBeside(const std::filesystem::path &target, const char *suffix);

// Whether `name` is one uniqueTempBeside() gives for a target named
// `targetName` with `suffix`.
bool isTempNameOf(const std::string &name, const std::string &targetName, const char *suffix);

// Whether something is under `path`, a dangling link included. nullopt
// when the filesystem would not say, with its reason in `error` (a name
// too long for it among them).
std::optional<bool> somethingAt(const std::filesystem::path &path, std::error_code *error = nullptr);

// Whether `a` and `b` name one file, as the filesystem compares names:
// when both are there, whether they are the same file; when neither is,
// whether they are in the same folder under names that differ at most in
// the case of ASCII letters (a filesystem that ignores case would take
// them as one). When one is there and the other is not, they are not one.
// nullopt when the filesystem would not say, with its reason in `error`:
// a question it cannot answer is neither "same" nor "not the same", and
// the callers refuse with that reason.
std::optional<bool> sameFile(const std::filesystem::path &a, const std::filesystem::path &b,
                             std::error_code *error = nullptr);

// Why the place for a file named `name` could not be looked at, in words:
// a name too long for the drive is said as such.
std::string lookError(const std::string &name, const std::error_code &error);

// After a rename through a hard link whose old name could not be removed
// (NoReplaceRename::oldNameRemains): removes `secondName` again, only while
// it is still the same file as `kept`. True when it is gone.
bool removeSecondName(const std::filesystem::path &secondName, const std::filesystem::path &kept);

// Opens the file at `path` for writing without creating it, flushes it to
// the device and answers its size. Throws when it is not there or cannot
// be flushed. Its permissions are left as they are.
std::uint64_t flushExistingFile(const std::filesystem::path &path);

// The size of the file at `path`; nullopt when it cannot be told.
std::optional<std::uint64_t> fileSizeOf(const std::filesystem::path &path);

// The regular files in `folder` whose names isTempNameOf() takes for
// `targetName` and `suffix`. Empty when the folder cannot be read.
std::vector<std::filesystem::path> tempsBeside(const std::filesystem::path &folder, const std::string &targetName,
                                               const char *suffix);

}  // namespace seabass::infrastructure::audio
