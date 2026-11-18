// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/paths/shared_read_stream.hpp"

#include <algorithm>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace seabass::infrastructure
{

#if defined(_WIN32)

namespace
{
// What a buffered read asks the system for at a time.
constexpr std::size_t BufferBytes = 64 * 1024;
}  // namespace

SharedReadBuffer::~SharedReadBuffer()
{
    close();
}

bool SharedReadBuffer::open(const std::filesystem::path &path, bool unbuffered)
{
    close();
    // Everything a std::ifstream allows, plus FILE_SHARE_DELETE: see
    // SharedReadStream.
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    m_handle = handle;
    m_unbuffered = unbuffered;
    // Unbuffered, the one byte is only for get() and peek(); read()
    // goes straight to the file, one request per call (xsgetn).
    m_buffer.assign(unbuffered ? 1 : BufferBytes, '\0');
    setg(m_buffer.data(), m_buffer.data(), m_buffer.data());
    return true;
}

void SharedReadBuffer::close()
{
    if (m_handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
    setg(nullptr, nullptr, nullptr);
}

std::optional<std::uintmax_t> SharedReadBuffer::size() const
{
    LARGE_INTEGER size{};
    if (m_handle == nullptr || !GetFileSizeEx(static_cast<HANDLE>(m_handle), &size)) {
        return std::nullopt;
    }
    return static_cast<std::uintmax_t>(size.QuadPart);
}

std::streamsize SharedReadBuffer::readSome(char *out, std::streamsize count)
{
    if (m_handle == nullptr || count <= 0) {
        return 0;
    }
    // A refused read gives 0 like the end of the file does; telling the
    // two apart is the caller's (FileEntrySource::readFailed()), as it is
    // for an ifstream, whose flags do not say it on every platform.
    const DWORD want = static_cast<DWORD>(std::min<std::streamsize>(count, 1 << 30));
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(m_handle), out, want, &got, nullptr)) {
        return 0;
    }
    return static_cast<std::streamsize>(got);
}

SharedReadBuffer::int_type SharedReadBuffer::underflow()
{
    if (gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }
    const std::streamsize got = readSome(m_buffer.data(), static_cast<std::streamsize>(m_buffer.size()));
    if (got <= 0) {
        setg(m_buffer.data(), m_buffer.data(), m_buffer.data());
        return traits_type::eof();
    }
    setg(m_buffer.data(), m_buffer.data(), m_buffer.data() + got);
    return traits_type::to_int_type(*gptr());
}

std::streamsize SharedReadBuffer::xsgetn(char_type *out, std::streamsize count)
{
    std::streamsize done = 0;
    while (done < count) {
        const std::streamsize buffered = egptr() - gptr();
        if (buffered > 0) {
            const std::streamsize take = std::min(buffered, count - done);
            std::memcpy(out + done, gptr(), static_cast<std::size_t>(take));
            gbump(static_cast<int>(take));
            done += take;
            continue;
        }
        const std::streamsize remaining = count - done;
        if (!m_unbuffered && remaining < static_cast<std::streamsize>(m_buffer.size())) {
            // A small request is served from a full buffer's read.
            if (traits_type::eq_int_type(underflow(), traits_type::eof())) {
                break;
            }
            continue;
        }
        // A large one, or any one unbuffered, goes straight to the file.
        const std::streamsize got = readSome(out + done, remaining);
        if (got <= 0) {
            break;
        }
        done += got;
    }
    return done;
}

SharedReadBuffer::pos_type SharedReadBuffer::seekoff(off_type offset, std::ios::seekdir dir, std::ios::openmode which)
{
    if (m_handle == nullptr || !(which & std::ios::in)) {
        return pos_type(off_type(-1));
    }
    LARGE_INTEGER zero{};
    LARGE_INTEGER filePosition{};
    if (!SetFilePointerEx(static_cast<HANDLE>(m_handle), zero, &filePosition, FILE_CURRENT)) {
        return pos_type(off_type(-1));
    }
    // Where the reader is: the file's position less what is still
    // buffered and unread.
    const off_type here = static_cast<off_type>(filePosition.QuadPart) - static_cast<off_type>(egptr() - gptr());
    off_type target = 0;
    if (dir == std::ios::beg) {
        target = offset;
    } else if (dir == std::ios::cur) {
        target = here + offset;
    } else {
        const auto end = size();
        if (!end) {
            return pos_type(off_type(-1));
        }
        target = static_cast<off_type>(*end) + offset;
    }
    if (target < 0) {
        return pos_type(off_type(-1));
    }
    LARGE_INTEGER to{};
    to.QuadPart = static_cast<LONGLONG>(target);
    if (!SetFilePointerEx(static_cast<HANDLE>(m_handle), to, nullptr, FILE_BEGIN)) {
        return pos_type(off_type(-1));
    }
    setg(m_buffer.data(), m_buffer.data(), m_buffer.data());
    return pos_type(target);
}

SharedReadBuffer::pos_type SharedReadBuffer::seekpos(pos_type position, std::ios::openmode which)
{
    return seekoff(off_type(position), std::ios::beg, which);
}

#endif

SharedReadStream::SharedReadStream() : std::istream(nullptr)
{
    rdbuf(&m_buf);
    setstate(std::ios::failbit);
}

SharedReadStream::SharedReadStream(const std::filesystem::path &path) : SharedReadStream()
{
    open(path);
}

SharedReadStream::~SharedReadStream()
{
    m_buf.close();
}

void SharedReadStream::open(const std::filesystem::path &path, bool unbuffered)
{
    m_buf.close();
    m_path = path;
    setstate(std::ios::failbit);
#if defined(_WIN32)
    if (!m_buf.open(path, unbuffered)) {
        return;
    }
#else
    // Before opening: libstdc++ takes a buffer only from a closed filebuf.
    if (unbuffered) {
        m_buf.pubsetbuf(nullptr, 0);
    }
    if (m_buf.open(path, std::ios::in | std::ios::binary) == nullptr) {
        return;
    }
#endif
    clear();
}

std::optional<std::uintmax_t> SharedReadStream::sizeNow() const
{
#if defined(_WIN32)
    return m_buf.size();
#else
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(m_path, ec);
    if (ec) {
        return std::nullopt;
    }
    return size;
#endif
}

}  // namespace seabass::infrastructure
