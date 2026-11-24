// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "infrastructure/hashing/sha256.hpp"

namespace seabass::infrastructure::track_import
{

struct FileCopyOptions
{
    application::CancellationToken cancel;
    // Called with the bytes written so far, after each piece.
    std::function<void(std::uint64_t)> progress;
    std::size_t chunkSize = std::size_t{1} << 20;
};

// What copyFileVerified did. Exactly one of the three is the answer:
// copied, cancelled, or an error.
struct FileCopyResult
{
    bool copied = false;
    bool cancelled = false;
    // Why not, in words for a result list. Empty when copied or cancelled.
    std::string error;
    // When copied: the file's size and the SHA-256 of its bytes, which is
    // what the destination was read back and found to hold.
    std::uint64_t bytes = 0;
    hashing::Sha256Digest sha256{};
};

// Copies one audio file onto a stick, for the track import.
//
// The bytes go to "<destination>.seabass-import-tmp" piece by piece, never
// all in memory (copyFileDurablyAtomic reads its whole source first and is
// for databases). When the source is read to its end the copy is flushed
// to the device, read back and compared with what was read from the
// source, given the source's modification time, and only then renamed to
// `destination`. So a file under the destination's name is always a whole
// one: a cancel, a full stick, a pulled stick or a source that changed
// while it was read leave at most the temporary file, which the next copy
// to that place removes.
//
// A source that was written to while it was read (its size or its
// modification time is another afterwards) is not imported: a tagger
// rewrites a tag in place, at the same size. A rewrite within one tick
// of the source filesystem's clock (2 s on FAT) is not seen.
//
// Refuses, touching nothing: a destination that exists (the import plan
// picks a free name; this never replaces a file), a place it could not
// look at, a source that is not a regular file. Folders above the
// destination are created, and taken away again, while empty, when the
// copy does not arrive.
//
// Two things are the caller's: that `destination` is inside the stick,
// and that nothing else writes the stick meanwhile (the stick's write
// lock). The last look at the destination and the rename are two steps,
// and two copies into one new folder at the same time may take that
// folder from each other: one copy at a time.
//
// The read-back is of the file as the operating system has it, which may
// be its cache rather than the device: it proves the temporary file holds
// these bytes and nothing else (a stale one that was not emptied would
// not), not that the stick's flash does.
//
// Never throws.
[[nodiscard]] FileCopyResult copyFileVerified(const std::filesystem::path &source,
                                              const std::filesystem::path &destination,
                                              const FileCopyOptions &options);

// The suffix of the temporary file, for a scan that tidies leftovers.
inline constexpr const char *ImportTempSuffix = ".seabass-import-tmp";

}  // namespace seabass::infrastructure::track_import
