// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// Shim for mingw-w64, which has no <byteswap.h> (that's Linux-specific).
// See endian.h in this same directory for the full explanation --
// mingw-w64's own <stdlib.h> already provides MSVC-compatible
// _byteswap_ushort/_byteswap_ulong/_byteswap_uint64 intrinsics (the same
// ones kaitaistream.cpp's own _MSC_VER branch uses), this just exposes
// them under the names the Linux/BSD branch expects.
#include <stdlib.h>

#define bswap_16(x) _byteswap_ushort(x)
#define bswap_32(x) _byteswap_ulong(x)
#define bswap_64(x) _byteswap_uint64(x)
