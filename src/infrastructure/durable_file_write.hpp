// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::infrastructure
{

// Atomically (same-directory temp file + rename) and durably (the temp
// file is fsync'd / FlushFileBuffers'd before the rename; the
// containing directory is fsync'd after the rename on POSIX -- the
// standard "durable rename" pattern) replaces the file at `path` with
// `data`. Works whether or not a file already exists at `path` -- no
// partial/truncated file is ever visible there, and a crash right after
// this returns true can't lose the write to a write-back cache, which
// matters for this project's actual medium: a USB stick that can be
// pulled at any moment.
//
// Returns false, leaving whatever was at `path` before (if anything)
// completely untouched, if the write, fsync, or rename failed.
bool writeFileDurablyAtomic(const std::string &path, const std::string &data);

// Same guarantee as writeFileDurablyAtomic(), but the new content comes
// from an existing file (sourcePath) instead of an in-memory buffer --
// for replacing a database file with a modified scratch copy of itself,
// where reading the whole thing into one std::string first and reusing
// the tested primitive above is simpler than a second streaming-copy
// implementation, and these files (library metadata, not embedded
// audio) are small enough that holding the whole thing in memory isn't
// a concern. Returns false, leaving targetPath untouched, if sourcePath
// can't be read.
bool copyFileDurablyAtomic(const std::string &sourcePath, const std::string &targetPath);

// The second half of the durable-rename pattern on its own: fsync the
// directory that contains `filePath`, so a rename/create/unlink of that
// entry survives a crash. No-op on Windows (MoveFileEx-based renames have
// no equivalent gap). Exposed for callers that do their own rename -- the
// stick backup's compaction replaces a multi-GB archive by rename and
// cannot go through writeFileDurablyAtomic()'s in-memory buffer.
void fsyncDirectoryContaining(const std::string &filePath);

}  // namespace seabass::infrastructure
