// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace seabass::infrastructure::hashing
{

using Sha1Digest = std::array<std::uint8_t, 20>;

// SHA-1 (FIPS 180-4). Not for anything that has to resist an attacker:
// only for naming things the way another program already names them (an
// Engine library's covers kept in its database). Pinned by the FIPS test
// vectors in tests/sha1_test.cpp.
Sha1Digest sha1(std::string_view data);

// Lowercase hex of any bytes, two characters each.
std::string toHex(std::span<const std::uint8_t> bytes);

}  // namespace seabass::infrastructure::hashing
