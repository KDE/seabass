// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/hashing/sha256.hpp"

#include <cassert>
#include <cstring>

namespace seabass::infrastructure::hashing
{

namespace
{

constexpr std::array<std::uint32_t, 64> RoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::array<std::uint32_t, 8> InitialState = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

inline std::uint32_t rotr(std::uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

inline std::uint32_t loadBigEndian32(const std::uint8_t *p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

}  // namespace

Sha256::Sha256() : m_state(InitialState)
{
}

void Sha256::processBlock(const std::uint8_t *block)
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = loadBigEndian32(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

    for (int i = 0; i < 64; ++i) {
        std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + s1 + ch + RoundConstants[static_cast<std::size_t>(i)] + w[i];
        std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(std::span<const std::byte> bytes)
{
    assert(!m_finished && "Sha256::update() after finish()");
    if (m_finished) {
        return;
    }
    const auto *p = reinterpret_cast<const std::uint8_t *>(bytes.data());
    std::size_t remaining = bytes.size();
    m_totalBytes += remaining;

    if (m_bufferLength > 0) {
        std::size_t take = std::min(remaining, m_buffer.size() - m_bufferLength);
        std::memcpy(m_buffer.data() + m_bufferLength, p, take);
        m_bufferLength += take;
        p += take;
        remaining -= take;
        if (m_bufferLength == m_buffer.size()) {
            processBlock(m_buffer.data());
            m_bufferLength = 0;
        }
    }
    while (remaining >= m_buffer.size()) {
        processBlock(p);
        p += m_buffer.size();
        remaining -= m_buffer.size();
    }
    if (remaining > 0) {
        std::memcpy(m_buffer.data(), p, remaining);
        m_bufferLength = remaining;
    }
}

void Sha256::update(std::string_view text)
{
    update(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

Sha256Digest Sha256::finish()
{
    if (m_finished) {
        assert(false && "Sha256::finish() called twice");
        return m_digest;
    }
    const std::uint64_t bitLength = m_totalBytes * 8;

    // Padding: a single 0x80, zeros up to 56 mod 64, then the 64-bit
    // big-endian message length in bits.
    m_buffer[m_bufferLength++] = 0x80;
    if (m_bufferLength > 56) {
        std::memset(m_buffer.data() + m_bufferLength, 0, m_buffer.size() - m_bufferLength);
        processBlock(m_buffer.data());
        m_bufferLength = 0;
    }
    std::memset(m_buffer.data() + m_bufferLength, 0, 56 - m_bufferLength);
    for (int i = 0; i < 8; ++i) {
        m_buffer[56 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bitLength >> (56 - 8 * i));
    }
    processBlock(m_buffer.data());

    for (std::size_t i = 0; i < 8; ++i) {
        m_digest[i * 4 + 0] = static_cast<std::uint8_t>(m_state[i] >> 24);
        m_digest[i * 4 + 1] = static_cast<std::uint8_t>(m_state[i] >> 16);
        m_digest[i * 4 + 2] = static_cast<std::uint8_t>(m_state[i] >> 8);
        m_digest[i * 4 + 3] = static_cast<std::uint8_t>(m_state[i]);
    }
    m_finished = true;
    return m_digest;
}

Sha256Digest Sha256::of(std::span<const std::byte> bytes)
{
    Sha256 hasher;
    hasher.update(bytes);
    return hasher.finish();
}

Sha256Digest Sha256::of(std::string_view text)
{
    Sha256 hasher;
    hasher.update(text);
    return hasher.finish();
}

std::string toHex(const Sha256Digest &digest)
{
    static constexpr char Alphabet[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
        out.push_back(Alphabet[byte >> 4]);
        out.push_back(Alphabet[byte & 0x0f]);
    }
    return out;
}

std::optional<Sha256Digest> digestFromHex(std::string_view hex)
{
    if (hex.size() != 64) {
        return std::nullopt;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Sha256Digest digest{};
    for (std::size_t i = 0; i < 32; ++i) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        digest[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return digest;
}

}  // namespace seabass::infrastructure::hashing
