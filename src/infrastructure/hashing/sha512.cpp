// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/hashing/sha512.hpp"

#include <algorithm>
#include <cstring>

namespace seabass::infrastructure::hashing
{
namespace
{

constexpr std::array<std::uint64_t, 80> K = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

constexpr std::uint64_t rotr(std::uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

}  // namespace

Sha512::Sha512()
    : m_state{0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
              0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL}
{
}

void Sha512::processBlock(const std::uint8_t *block)
{
    std::array<std::uint64_t, 80> w{};
    for (int i = 0; i < 16; ++i) {
        std::uint64_t v = 0;
        for (int b = 0; b < 8; ++b) {
            v = (v << 8) | block[i * 8 + b];
        }
        w[static_cast<std::size_t>(i)] = v;
    }
    for (std::size_t i = 16; i < 80; ++i) {
        const std::uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const std::uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint64_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    std::uint64_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (std::size_t i = 0; i < 80; ++i) {
        const std::uint64_t S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
        const std::uint64_t ch = (e & f) ^ (~e & g);
        const std::uint64_t t1 = h + S1 + ch + K[i] + w[i];
        const std::uint64_t S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
        const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint64_t t2 = S0 + maj;
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

void Sha512::update(std::span<const std::uint8_t> bytes)
{
    m_totalBytes += bytes.size();
    std::size_t at = 0;
    while (at < bytes.size()) {
        const std::size_t take = std::min(bytes.size() - at, m_buffer.size() - m_bufferLength);
        std::memcpy(m_buffer.data() + m_bufferLength, bytes.data() + at, take);
        m_bufferLength += take;
        at += take;
        if (m_bufferLength == m_buffer.size()) {
            processBlock(m_buffer.data());
            m_bufferLength = 0;
        }
    }
}

void Sha512::update(std::string_view text)
{
    update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

Sha512Digest Sha512::finish()
{
    // The length in bits as a 128-bit big-endian number; the high half
    // is zero for anything under 2^61 bytes.
    const std::uint64_t bits = m_totalBytes * 8;
    std::array<std::uint8_t, 144> pad{};
    pad[0] = 0x80;
    const std::size_t used = m_bufferLength;
    const std::size_t padLength = (used < 112 ? 112 - used : 240 - used);
    std::array<std::uint8_t, 16> length{};
    for (int i = 0; i < 8; ++i) {
        length[static_cast<std::size_t>(15 - i)] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    const std::uint64_t total = m_totalBytes;
    update(std::span<const std::uint8_t>(pad.data(), padLength));
    update(std::span<const std::uint8_t>(length.data(), length.size()));
    m_totalBytes = total;
    Sha512Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (int b = 0; b < 8; ++b) {
            digest[i * 8 + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(m_state[i] >> (56 - 8 * b));
        }
    }
    return digest;
}

Sha512Digest Sha512::of(std::span<const std::uint8_t> bytes)
{
    Sha512 h;
    h.update(bytes);
    return h.finish();
}

Sha512Digest Sha512::of(std::string_view text)
{
    Sha512 h;
    h.update(text);
    return h.finish();
}

namespace
{

// HMAC-SHA512 with the key's inner and outer pads hashed once: PBKDF2
// calls it hundreds of thousands of times with one key.
class HmacSha512
{
public:
    explicit HmacSha512(std::string_view key)
    {
        std::array<std::uint8_t, 128> block{};
        if (key.size() > block.size()) {
            const Sha512Digest hashed = Sha512::of(key);
            std::copy(hashed.begin(), hashed.end(), block.begin());
        } else {
            std::memcpy(block.data(), key.data(), key.size());
        }
        std::array<std::uint8_t, 128> inner{}, outer{};
        for (std::size_t i = 0; i < block.size(); ++i) {
            inner[i] = block[i] ^ 0x36;
            outer[i] = block[i] ^ 0x5c;
        }
        m_inner.update(std::span<const std::uint8_t>(inner));
        m_outer.update(std::span<const std::uint8_t>(outer));
    }
    Sha512Digest of(std::span<const std::uint8_t> message) const
    {
        Sha512 inner = m_inner;
        inner.update(message);
        const Sha512Digest innerDigest = inner.finish();
        Sha512 outer = m_outer;
        outer.update(std::span<const std::uint8_t>(innerDigest));
        return outer.finish();
    }

private:
    Sha512 m_inner;
    Sha512 m_outer;
};

}  // namespace

std::vector<std::uint8_t> pbkdf2HmacSha512(std::string_view password, std::span<const std::uint8_t> salt,
                                           std::uint32_t iterations, std::size_t length)
{
    const HmacSha512 prf(password);
    std::vector<std::uint8_t> out;
    out.reserve(length);
    for (std::uint32_t blockIndex = 1; out.size() < length; ++blockIndex) {
        std::vector<std::uint8_t> first(salt.begin(), salt.end());
        for (int b = 3; b >= 0; --b) {
            first.push_back(static_cast<std::uint8_t>(blockIndex >> (8 * b)));
        }
        Sha512Digest u = prf.of(first);
        Sha512Digest t = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            u = prf.of(std::span<const std::uint8_t>(u));
            for (std::size_t j = 0; j < t.size(); ++j) {
                t[j] ^= u[j];
            }
        }
        const std::size_t take = std::min(t.size(), length - out.size());
        out.insert(out.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return out;
}

}  // namespace seabass::infrastructure::hashing
