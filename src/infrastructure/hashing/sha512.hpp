// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace seabass::infrastructure::hashing
{

using Sha512Digest = std::array<std::uint8_t, 64>;

// Plain-C++ SHA-512 (FIPS 180-4), for the one thing that needs it:
// deriving OneLibrary's SQLCipher key once per database instead of on
// every open (see pbkdf2HmacSha512). Written here for the reason Sha256
// is: seabass_core has no crypto dependency of its own. Pinned by the
// FIPS and RFC test vectors in tests/sha512_test.cpp.
class Sha512
{
public:
    Sha512();

    void update(std::span<const std::uint8_t> bytes);
    void update(std::string_view text);
    Sha512Digest finish();

    static Sha512Digest of(std::span<const std::uint8_t> bytes);
    static Sha512Digest of(std::string_view text);

private:
    void processBlock(const std::uint8_t *block);

    std::array<std::uint64_t, 8> m_state{};
    std::array<std::uint8_t, 128> m_buffer{};
    std::size_t m_bufferLength = 0;
    std::uint64_t m_totalBytes = 0;
};

// PBKDF2 (RFC 8018) with HMAC-SHA512: `length` bytes of key from
// `password` and `salt` after `iterations` rounds.
std::vector<std::uint8_t> pbkdf2HmacSha512(std::string_view password, std::span<const std::uint8_t> salt,
                                           std::uint32_t iterations, std::size_t length);

}  // namespace seabass::infrastructure::hashing
