// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <streambuf>
#include <vector>

namespace seabass::infrastructure
{

#if defined(_WIN32)
// The Windows side of SharedReadStream: a read-only stream buffer
// straight over a file handle, ReadFile and SetFilePointerEx. Its own,
// because no standard library opens a file with FILE_SHARE_DELETE, and
// the ways to hand a stream a handle opened elsewhere differ by toolchain
// (MSVC's filebuf(FILE*), libstdc++'s stdio_filebuf): MSVC and MinGW
// both build this. The handle is a void * so that this header does not
// pull in <windows.h>.
class SharedReadBuffer : public std::streambuf
{
public:
    SharedReadBuffer() = default;
    ~SharedReadBuffer() override;
    SharedReadBuffer(const SharedReadBuffer &) = delete;
    SharedReadBuffer &operator=(const SharedReadBuffer &) = delete;

    bool open(const std::filesystem::path &path, bool unbuffered);
    void close();
    bool isOpen() const { return m_handle != nullptr; }
    std::optional<std::uintmax_t> size() const;

protected:
    int_type underflow() override;
    std::streamsize xsgetn(char_type *out, std::streamsize count) override;
    pos_type seekoff(off_type offset, std::ios::seekdir dir, std::ios::openmode which) override;
    pos_type seekpos(pos_type position, std::ios::openmode which) override;

private:
    // One ReadFile; the bytes it gave, 0 at the end of the file or when
    // the device refused.
    std::streamsize readSome(char *out, std::streamsize count);

    void *m_handle = nullptr;
    std::vector<char> m_buffer;
    bool m_unbuffered = false;
};
#endif

// A file opened for binary reading that does not stand in the way of
// anyone else: while it is open, the file can still be written, deleted
// and renamed away, as on POSIX. (Renaming another file ONTO it is still
// refused on Windows: its default rename does not replace an open file,
// whatever the reader allows.)
//
// On Windows a std::ifstream opens a file without FILE_SHARE_DELETE, so
// for as long as it is open nobody can delete or rename it. Reading a
// stick's database while a writer commits is exactly that: SQLite
// deletes its rollback journal at the end of every transaction, the
// delete is refused while a backup is reading the journal, and the
// WRITER fails, "disk I/O error" (backup_database_capture_test case 4 on
// Windows, 6 of 10 runs). A backup must never be the reason another
// program's write fails, so every read of a stick file goes through this.
//
// A file deleted while open here stays readable through this stream, as
// on POSIX; by its path it is then "delete pending" on Windows, and every
// stat and open of the path is refused with access denied until the last
// handle closes. sizeNow() asks the open file, not the path, for that
// reason.
class SharedReadStream : public std::istream
{
public:
    SharedReadStream();
    // Opens at once; check with operator bool, as for an ifstream.
    explicit SharedReadStream(const std::filesystem::path &path);
    ~SharedReadStream() override;

    SharedReadStream(const SharedReadStream &) = delete;
    SharedReadStream &operator=(const SharedReadStream &) = delete;

    // Opens `path`; `unbuffered` makes every read one request to the
    // system, for reading page by page after a failure. Sets failbit if
    // the file could not be opened.
    void open(const std::filesystem::path &path, bool unbuffered = false);

    // The size of the open file now, or nothing if it cannot be told.
    // On Windows from the open handle, so a file that was deleted while
    // open still answers; elsewhere from the path, as before.
    std::optional<std::uintmax_t> sizeNow() const;

private:
    std::filesystem::path m_path;
#if defined(_WIN32)
    SharedReadBuffer m_buf;
#else
    std::filebuf m_buf;
#endif
};

}  // namespace seabass::infrastructure
