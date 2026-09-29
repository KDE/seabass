// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// SHA-1 against the FIPS 180 known answers, and the hex spelling.

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/hashing/sha1.hpp"

using namespace seabass::infrastructure::hashing;

int main()
{
    assert(toHex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    assert(toHex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    assert(toHex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
           == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    assert(toHex(sha1(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    const std::vector<std::uint8_t> bytes = {0x00, 0x0f, 0xa5, 0xff};
    assert(toHex(bytes) == "000fa5ff");
    std::cout << "sha1_test: all cases passed\n";
    return 0;
}
