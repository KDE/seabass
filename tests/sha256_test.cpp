// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <string>

#include "infrastructure/hashing/sha256.hpp"

using namespace seabass::infrastructure::hashing;

namespace
{

std::string hexOf(std::string_view text)
{
    return toHex(Sha256::of(text));
}

}  // namespace

int main()
{
    // FIPS 180-4 / NIST CAVP vectors. If any of these fail the whole
    // manifest scheme is worthless, so they come first.
    assert(hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::cout << "case 1 (empty message) OK\n";

    assert(hexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::cout << "case 2 (\"abc\") OK\n";

    // 56 bytes: exercises the "padding does not fit, needs a second block" path.
    assert(hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
           == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::cout << "case 3 (56-byte two-block message) OK\n";

    // Exactly one block of input (64 bytes) -- padding must go entirely
    // into a fresh block.
    assert(hexOf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno")
           == "2ff100b36c386c65a1afc462ad53e25479bec9498ed00aa5a04de584bc25301b");
    std::cout << "case 4 (exactly 64 bytes) OK\n";

    {
        Sha256 hasher;
        std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) {
            hasher.update(chunk);
        }
        assert(toHex(hasher.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        std::cout << "case 5 (one million 'a', streamed in 1000-byte chunks) OK\n";
    }

    // Chunk boundaries must not matter: feed the same bytes in every
    // awkward split (1 + rest, 63 + rest, 64 + rest, 65 + rest, byte-by-byte).
    {
        std::string message;
        for (int i = 0; i < 300; ++i) {
            message.push_back(static_cast<char>('A' + (i * 7) % 26));
        }
        Sha256Digest whole = Sha256::of(message);
        for (std::size_t split : {std::size_t(1), std::size_t(63), std::size_t(64), std::size_t(65), std::size_t(127), std::size_t(128)}) {
            Sha256 hasher;
            hasher.update(std::string_view(message).substr(0, split));
            hasher.update(std::string_view(message).substr(split));
            assert(hasher.finish() == whole);
        }
        Sha256 byteWise;
        for (char c : message) {
            byteWise.update(std::string_view(&c, 1));
        }
        assert(byteWise.finish() == whole);
        std::cout << "case 6 (chunking is invisible) OK\n";
    }

    {
        Sha256Digest digest = Sha256::of("abc");
        std::string hex = toHex(digest);
        assert(hex.size() == 64);
        auto parsed = digestFromHex(hex);
        assert(parsed.has_value() && *parsed == digest);
        std::string upper = hex;
        for (char &c : upper) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        assert(digestFromHex(upper) == digest);
        assert(!digestFromHex(hex.substr(1)).has_value());
        assert(!digestFromHex(hex + "0").has_value());
        std::string bad = hex;
        bad[10] = 'g';
        assert(!digestFromHex(bad).has_value());
        std::cout << "case 7 (hex round trip, rejects malformed) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
