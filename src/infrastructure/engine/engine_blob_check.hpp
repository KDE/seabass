// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::infrastructure::engine::internal
{

// Whether libdjinterop's zlib_uncompress can be handed `bytes` safely: a
// four-byte big-endian length, then a zlib stream. As pinned, it takes the
// address of byte 4 even when the blob has only four (out of range: an
// abort under MSVC's checked iterators that no catch sees), gives inflate
// four bytes more than the blob holds, and loops for ever on a stream that
// does not end within them (inflate then returns Z_BUF_ERROR with no input
// left, which it does not treat as the end). So a blob is passed on only
// when it is empty or says length 0 (both decode to nothing without a
// read past the header), or when its stream, inflated here first, ends
// within the blob and inflates to no more than a sane bound (the length
// is also reserved up front). Defined in libdjinterop_engine_cue_writer.cpp.
bool engineBlobDecodable(const std::string &bytes);

}  // namespace seabass::infrastructure::engine::internal
