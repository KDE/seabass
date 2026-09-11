// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace seabass::infrastructure::hashing
{

using Sha256Digest = std::array<std::uint8_t, 32>;

// Plain-C++ SHA-256 (FIPS 180-4), streaming. Written here rather than
// pulled in because nothing in seabass_core links Qt (QCryptographicHash
// lives in the GUI only) and the project has no other crypto dependency;
// the stick backup's manifest needs a strong per-file hash computed while
// bytes stream past, which zlib's CRC32 is not. Correctness is pinned by
// the FIPS test vectors in tests/sha256_test.cpp.
class Sha256
{
public:
    Sha256();

    void update(std::span<const std::byte> bytes);
    void update(std::string_view text);

    // Finalizes and returns the digest. The object is spent afterwards:
    // calling update() or finish() again is a logic error (asserted in
    // debug builds, returns the same digest in release).
    Sha256Digest finish();

    static Sha256Digest of(std::span<const std::byte> bytes);
    static Sha256Digest of(std::string_view text);

private:
    void processBlock(const std::uint8_t *block);

    std::array<std::uint32_t, 8> m_state{};
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_bufferLength = 0;
    std::uint64_t m_totalBytes = 0;
    bool m_finished = false;
    Sha256Digest m_digest{};
};

// Lowercase hex, 64 characters.
std::string toHex(const Sha256Digest &digest);

// Accepts exactly 64 hex characters of either case; anything else -> nullopt.
std::optional<Sha256Digest> digestFromHex(std::string_view hex);

}  // namespace seabass::infrastructure::hashing
