// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/hashing/sha1.hpp"

namespace seabass::infrastructure::hashing
{

namespace
{

std::uint32_t rotateLeft(std::uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

}  // namespace

Sha1Digest sha1(std::string_view data)
{
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string message(data);
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) {
        message.push_back('\0');
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    }
    for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto byte = [&](int k) {
                return static_cast<std::uint32_t>(static_cast<unsigned char>(message[chunk + 4 * i + k]));
            };
            w[i] = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = rotateLeft(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    Sha1Digest digest{};
    for (int i = 0; i < 5; ++i) {
        for (int k = 0; k < 4; ++k) {
            digest[static_cast<size_t>(4 * i + k)] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * k));
        }
    }
    return digest;
}

std::string toHex(std::span<const std::uint8_t> bytes)
{
    static constexpr char Alphabet[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(Alphabet[byte >> 4]);
        out.push_back(Alphabet[byte & 0x0f]);
    }
    return out;
}

}  // namespace seabass::infrastructure::hashing
