// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// SHA-512 and PBKDF2-HMAC-SHA512 against independent references: the FIPS
// 180-4 vectors, and values from Python's hashlib (OpenSSL underneath).
// OneLibrary's SQLCipher key is derived with these, so a wrong byte here
// is a stick Seabass cannot open.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/hashing/sha512.hpp"

using namespace seabass::infrastructure::hashing;

namespace
{

std::string hex(const std::uint8_t *data, std::size_t size)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

std::string sha(std::string_view text)
{
    const Sha512Digest d = Sha512::of(text);
    return hex(d.data(), d.size());
}

std::string pbkdf2(std::string_view password, const std::vector<std::uint8_t> &salt, std::uint32_t iterations,
                   std::size_t length)
{
    const auto key = pbkdf2HmacSha512(password, salt, iterations, length);
    return hex(key.data(), key.size());
}

std::vector<std::uint8_t> bytesOf(std::string_view text)
{
    return {text.begin(), text.end()};
}

}  // namespace

int main()
{
    assert(sha("") == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    assert(sha("abc") == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    assert(sha("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")
           == "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    std::cout << "case 1 (FIPS 180-4 vectors) OK\n";

    // Either side of the padding boundary (112 bytes leave no room for the
    // length in the last block), and a message of several blocks.
    assert(sha(std::string(111, 'a')) == "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef86818196921760b4beff48404df811b953828274461673c68d04e297b0eb7b2b4d60fc6b566a2");
    assert(sha(std::string(112, 'a')) == "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32bd05f0f1ba33e568b88fd2d970929b719ecbb152f58f130a407c8830604b70ca");
    assert(sha(std::string(1000, 'a')) == "67ba5535a46e3f86dbfbed8cbbaf0125c76ed549ff8b0b9e03e0c88cf90fa634fa7b12b47d77b694de488ace8d9a65967dc96df599727d3292a8d9d447709c97");
    {
        Sha512 streamed;
        for (int i = 0; i < 1000; ++i) {
            streamed.update("a");
        }
        const Sha512Digest d = streamed.finish();
        assert(hex(d.data(), d.size()) == sha(std::string(1000, 'a')) && "byte by byte is the same as at once");
    }
    std::cout << "case 2 (padding boundaries, several blocks, streamed) OK\n";

    assert(pbkdf2("password", bytesOf("salt"), 1, 64) == "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce");
    assert(pbkdf2("password", bytesOf("salt"), 2, 64) == "e1d9c16aa681708a45f5c7c4e215ceb66e011a2e9f0040713f18aefdb866d53cf76cab2868a39b9f7840edce4fef5a82be67335c77a6068e04112754f27ccf4e");
    // Longer than one digest: a second block of output.
    assert(pbkdf2("passwordPASSWORDpassword", bytesOf("saltSALTsaltSALTsaltSALTsaltSALTsalt"), 4096, 80)
           == "8c0511f4c6e597c6ac6315d8f0362e225f3c501495ba23b868c005174dc4ee71115b59f9e60cd9532fa33e0f75aefe30225c583a186cd82bd4daea9724a3d3b804f75bdd41494fa324cab24bcc680fb3");
    std::vector<std::uint8_t> salt16;
    for (std::uint8_t i = 0; i < 16; ++i) {
        salt16.push_back(i);
    }
    // A password longer than SHA-512's block, which HMAC hashes first.
    assert(pbkdf2(std::string(200, 'k'), salt16, 1000, 32) == "27f7315eb771a8e4f035623424256d270ccea55feca4576930f25556ff73fac3");
    std::cout << "case 3 (PBKDF2-HMAC-SHA512) OK\n";

    // SQLCipher 4's own parameters: 256000 rounds, a 16-byte salt, 32 bytes.
    assert(pbkdf2("seabass", salt16, 256000, 32) == "35cdbce2217a237f84584e8fe49574a9e51508911e81e8884c04fabb0ae21f27");
    std::cout << "case 4 (SQLCipher 4's parameters) OK\n";
    return 0;
}
