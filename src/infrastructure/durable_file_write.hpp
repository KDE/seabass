// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

// Appends `data` to the file at `path`, creating it if needed, and
// makes it durable before returning: the write goes through O_APPEND
// (so a second process appending to the same file cannot overwrite this
// line) and is fsync'd, and a file this call created also gets its
// directory entry flushed. Returns false if any of that failed.
//
// For the append-only records this project keeps ON a stick, where the
// medium can be pulled between the write and the page cache reaching
// it: a line saying an audio file has been orphaned is worth nothing if
// the catalog edit is durable and the line is not.
bool appendToFileDurably(const std::string &path, const std::string &data);

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

// Writes out everything still pending for the filesystem `path` is on, and
// returns once it is on the device. The durable writes above each flush
// their own file, but a save also writes things that are not worth an
// fsync apiece and are megabytes together: the backup archive, the log,
// the caches. Left to the kernel, those were written when the stick was
// unmounted, which is where the DJ then waited, with no progress and no
// idea why eject took half a minute. Asked for at the end of a save
// instead, under "Committing changes to the stick". False where the
// platform offers no such call (Windows without elevation) or it failed;
// nothing depends on the answer.
bool flushFilesystemOf(const std::string &path);

// Whether `path` names one of rekordbox's analysis files: a name ending in
// .DAT, .EXT or .2EX, in any case. Where they sit is not asked: a false
// match only costs a cached analysis being read again.
bool isAnalysisFilePath(const std::string &path);
// The one place every write of an analysis file in this process is told,
// so that what is kept of an analysis on the computer (gui::
// TrackAnalysisCache) never outlives the file. writeFileDurablyAtomic()
// and copyFileDurablyAtomic() call it for every analysis file they
// replace, which covers every writer of one in src/ (the test
// analysis_file_writers_test lists the others that write files, and fails
// for one it does not know); a writer that places a file some other way
// calls it itself. A cache cannot rely on the file's time for this: FAT
// keeps it to two seconds, and a rewrite of the same size inside one step
// looks like the file it replaced.
void noteAnalysisFileWritten(const std::string &path);
// How many analysis file writes have been noted in this process: a reader
// that remembers the number sees whether any write came since.
std::uint64_t analysisFileWriteSerial();

// How many noted paths are held for readers that have not taken them. A
// write noted past this many forgets them all, and every reader that had
// not taken them is told that anything may have changed. A full stick
// restore writes more analysis files than this, and after it nothing
// kept of the stick's analyses is worth keeping anyway.
constexpr std::size_t AnalysisFileWritesHeld = 20000;
// The paths held right now, for the test.
std::size_t analysisFileWritesHeldNow();

// A reader of the analysis file writes noted in this process. Every
// reader is handed every write noted from the start of the process (a
// cache made late still forgets what was written before it), each
// once, or where some of them are no longer held, told that anything
// may have changed. A path is held until every reader alive has taken
// it, and never more than AnalysisFileWritesHeld paths in all, readers
// alive or not.
class AnalysisFileWriteReader
{
public:
    AnalysisFileWriteReader();
    ~AnalysisFileWriteReader();
    AnalysisFileWriteReader(const AnalysisFileWriteReader &) = delete;
    AnalysisFileWriteReader &operator=(const AnalysisFileWriteReader &) = delete;

    struct Writes
    {
        // In the order they were noted.
        std::vector<std::string> paths;
        // Writes this reader had not taken were forgotten (the cap, or
        // other readers took them before this one existed): any analysis
        // file may have changed.
        bool everything = false;
    };
    // Whether a write was noted that take() has not handed over: one
    // atomic read, no lock.
    bool behind() const;
    // The writes noted since the last take(), and lets go of them.
    Writes take();

private:
    // The serial up to which this reader has taken the writes.
    std::uint64_t m_seen = 0;
};

}  // namespace seabass::infrastructure
