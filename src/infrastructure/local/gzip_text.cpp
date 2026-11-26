// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/gzip_text.hpp"

#include <climits>

#include <zlib.h>

namespace seabass::infrastructure::local
{

std::optional<std::string> gzipBytes(const std::string &data, std::string *problem)
{
    if (data.size() > static_cast<std::size_t>(UINT_MAX / 2)) {
        *problem = "the text is too large to compress";
        return std::nullopt;
    }
    z_stream stream{};
    // 15 + 16: a gzip wrapper rather than zlib's, so the .gz is one.
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        *problem = "zlib could not start compressing";
        return std::nullopt;
    }
    std::string out(deflateBound(&stream, static_cast<uLong>(data.size())) + 32, '\0');
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    const std::size_t written = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        *problem = "zlib could not compress the text (code " + std::to_string(result) + ")";
        return std::nullopt;
    }
    out.resize(written);
    return out;
}

bool gunzip(std::istream &in, bool stopAtNewline, std::size_t limit, std::string *out, std::string *problem)
{
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        *problem = "zlib could not start decompressing";
        return false;
    }
    struct End
    {
        z_stream *s;
        ~End() { inflateEnd(s); }
    } end{&stream};

    char input[16384];
    char output[65536];
    bool streamEnded = false;
    while (!streamEnded) {
        if (stream.avail_in == 0) {
            in.read(input, sizeof input);
            const std::streamsize got = in.gcount();
            if (in.bad()) {
                *problem = "reading it failed";
                return false;
            }
            if (got == 0) {
                *problem = out->empty() ? "it is empty" : "it is truncated (the compressed data stops early)";
                return false;
            }
            stream.next_in = reinterpret_cast<Bytef *>(input);
            stream.avail_in = static_cast<uInt>(got);
        }
        // Again while input is left, or while the last call filled the
        // output buffer: inflate may hold output it had no room for.
        do {
            stream.next_out = reinterpret_cast<Bytef *>(output);
            stream.avail_out = sizeof output;
            const int result = inflate(&stream, Z_NO_FLUSH);
            if (result == Z_STREAM_END) {
                streamEnded = true;
            } else if (result == Z_BUF_ERROR) {
                // No progress possible without more input; with input
                // still unread that cannot happen to intact data.
                if (stream.avail_in > 0) {
                    *problem = "it is not intact gzip data (zlib made no progress)";
                    return false;
                }
                break;
            } else if (result != Z_OK) {
                *problem = "it is not intact gzip data (zlib code " + std::to_string(result) + ")";
                return false;
            }
            const std::size_t produced = sizeof output - stream.avail_out;
            const std::size_t before = out->size();
            out->append(output, produced);
            if (out->size() > limit) {
                *problem = "it inflates to more than " + std::to_string(limit) + " bytes";
                return false;
            }
            if (stopAtNewline && out->find('\n', before) != std::string::npos) {
                return true;
            }
        } while (!streamEnded && (stream.avail_in > 0 || stream.avail_out == 0));
    }
    if (stopAtNewline) {
        return true;  // the caller says there was no newline
    }
    if (stream.avail_in > 0 || in.peek() != std::char_traits<char>::eof()) {
        *problem = "it has bytes after the end of the compressed data";
        return false;
    }
    return true;
}

}  // namespace seabass::infrastructure::local
