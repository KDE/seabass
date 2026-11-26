// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <istream>
#include <optional>
#include <string>

// The gzip of Seabass's own records on the stick (the rekordbox baseline,
// the analysis ledger): one gzip member, so `zcat` reads the file on a
// machine that has never heard of Seabass.
namespace seabass::infrastructure::local
{

// `data` as one gzip member; nullopt with `problem` set when zlib refuses
// or the text is too large for one call.
std::optional<std::string> gzipBytes(const std::string &data, std::string *problem);

// Inflates the gzip stream in `in` into `out`. With `stopAtNewline`, stops
// as soon as `out` holds a newline and reports success without reading
// further; otherwise inflates to the end and refuses a stream that stops
// short, carries a bad gzip checksum, or has bytes after its end. Refuses
// more than `limit` bytes of output. `problem` says what is wrong in words
// that follow a file's name ("it is truncated ...").
bool gunzip(std::istream &in, bool stopAtNewline, std::size_t limit, std::string *out, std::string *problem);

}  // namespace seabass::infrastructure::local
