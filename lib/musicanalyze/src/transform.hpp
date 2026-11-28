// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// The transform the grid and the key share (a power of two long, in
// place), and the pair of values it and the grid's filters work with.
// Not part of what the library offers: its own sources only.

#include <algorithm>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

namespace musicanalyze::internal
{

// Two values worked on as one: the bands are filtered side by side and
// the butterflies of a transform done two at a time, two values to an
// instruction where the processor has them (SSE2, NEON). Each of the two
// is worked out exactly as it would be alone, so nothing changes in any
// value. Where the compiler has no such type, a pair of doubles
// with the same arithmetic.
#if defined(__GNUC__) || defined(__clang__)
using Pair = double __attribute__((vector_size(16)));
#else
struct Pair
{
    double first;
    double second;
};
inline Pair operator*(Pair a, Pair b)
{
    return {a.first * b.first, a.second * b.second};
}
inline Pair operator+(Pair a, Pair b)
{
    return {a.first + b.first, a.second + b.second};
}
inline Pair operator-(Pair a, Pair b)
{
    return {a.first - b.first, a.second - b.second};
}
#endif

inline Pair pairAt(const double *from)
{
    Pair pair;
    std::memcpy(&pair, from, sizeof pair);
    return pair;
}
inline void putPair(double *to, Pair pair)
{
    std::memcpy(to, &pair, sizeof pair);
}

// What a transform of one length and one direction is done with. The
// twiddles, each stage's own in a row: the stage of butterflies `half`
// apart has its `half` values from index half - 1 on, real parts and
// imaginary parts apart. And the order the butterflies want the values
// in: value i of it is value order[i] of what is transformed.
struct Twiddles
{
    std::size_t length = 0;
    bool inverse = false;
    std::vector<double> re;
    std::vector<double> im;
    std::vector<std::uint32_t> order;
};

// Makes `twiddles` those of a transform of n values that way. Every
// twiddle is worked out from its angle (one built by repeated
// multiplication drifts), once: the stages share them, a shorter stage
// using every so-many-th of the longest one's.
inline void prepare(Twiddles &twiddles, std::size_t n, bool inverse)
{
    if (twiddles.length == n && twiddles.inverse == inverse) {
        return;
    }
    if (twiddles.length == n) {
        // The other way round has the same angles with the other sign:
        // the same cosines, and the sines with theirs turned, which is
        // what working them out again gives (a sine is odd to the bit),
        // without a sine and a cosine a value.
        for (double &value : twiddles.im) {
            value = -value;
        }
        twiddles.inverse = inverse;
        return;
    }
    twiddles.length = n;
    twiddles.inverse = inverse;
    twiddles.re.resize(n - 1);
    twiddles.im.resize(n - 1);
    const double sign = inverse ? 1.0 : -1.0;
    const std::size_t longest = n / 2;
    for (std::size_t k = 0; k < longest; ++k) {
        const std::complex<double> value =
            std::polar(1.0, sign * 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n));
        twiddles.re[longest - 1 + k] = value.real();
        twiddles.im[longest - 1 + k] = value.imag();
    }
    for (std::size_t half = 1; half < longest; half <<= 1) {
        const std::size_t stride = longest / half;
        for (std::size_t k = 0; k < half; ++k) {
            twiddles.re[half - 1 + k] = twiddles.re[longest - 1 + k * stride];
            twiddles.im[half - 1 + k] = twiddles.im[longest - 1 + k * stride];
        }
    }
    // Counting with the bits the other way round. Worked out once and
    // kept: found again for every value of every transform, it took
    // more of a transform's time than its arithmetic did (a guess a bit
    // that the processor gets wrong half the time).
    twiddles.order.resize(n);
    for (std::size_t i = 0, j = 0; i < n; ++i) {
        twiddles.order[i] = static_cast<std::uint32_t>(j);
        std::size_t bit = n >> 1;
        for (; bit != 0 && (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
    }
}

// What a caller with two transforms of one long length (there and back)
// lends them, so the second need not work out what the first did.
struct TransformRoom
{
    Twiddles twiddles;
    std::vector<double> re;
    std::vector<double> im;
};

// The stages of a transform whose butterflies are `fromHalf` apart and
// more, up to those of pieces `upTo` long, over `n` values in the order
// the butterflies want them.
inline void stages(double *re, double *im, std::size_t n, std::size_t fromHalf, std::size_t upTo, const Twiddles &twiddles)
{
    for (std::size_t half = fromHalf; half < upTo; half <<= 1) {
        const double *const wr = twiddles.re.data() + (half - 1);
        const double *const wi = twiddles.im.data() + (half - 1);
        for (std::size_t start = 0; start < n; start += 2 * half) {
            double *const ar = re + start;
            double *const ai = im + start;
            double *const br = ar + half;
            double *const bi = ai + half;
            if (half == 1) {
                const double tr = wr[0] * br[0] - wi[0] * bi[0];
                const double ti = wr[0] * bi[0] + wi[0] * br[0];
                br[0] = ar[0] - tr;
                bi[0] = ai[0] - ti;
                ar[0] += tr;
                ai[0] += ti;
                continue;
            }
            for (std::size_t k = 0; k < half; k += 2) {
                const Pair lowRe = pairAt(ar + k);
                const Pair lowIm = pairAt(ai + k);
                const Pair highRe = pairAt(br + k);
                const Pair highIm = pairAt(bi + k);
                const Pair turnRe = pairAt(wr + k);
                const Pair turnIm = pairAt(wi + k);
                const Pair tr = turnRe * highRe - turnIm * highIm;
                const Pair ti = turnRe * highIm + turnIm * highRe;
                putPair(br + k, lowRe - tr);
                putPair(bi + k, lowIm - ti);
                putPair(ar + k, lowRe + tr);
                putPair(ai + k, lowIm + ti);
            }
        }
    }
}

// Every stage of a transform, over n values in the order the butterflies
// want them (Twiddles::order), real and imaginary parts in rows.
//
// The stages whose butterflies are close together are done a piece
// of the values at a time, all of them on one piece before the next:
// a piece stays in the processor's nearest memory for as long as it
// is worked on, where a stage at a time over all the values fetches
// every one of them from further away for every stage. No butterfly
// of such a stage reaches out of its piece, so each still gets what
// the stages before it made of its two values.
inline void transformRows(double *re, double *im, std::size_t n, const Twiddles &twiddles)
{
    constexpr std::size_t PieceValues = 1024;
    const std::size_t piece = std::min(n, PieceValues);
    for (std::size_t from = 0; from < n; from += piece) {
        stages(re + from, im + from, piece, 1, piece, twiddles);
    }
    stages(re, im, n, piece, n, twiddles);
}

}  // namespace musicanalyze::internal
