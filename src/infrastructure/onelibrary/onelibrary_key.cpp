// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/onelibrary/onelibrary_key.hpp"

#include "infrastructure/hashing/sha512.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <mutex>

#include <cstdint>
#include <stdexcept>
#include <vector>
#include <zlib.h>

namespace seabass::infrastructure::onelibrary
{

namespace
{

// Python's base64.b85decode alphabet -- NOT the RFC 1924/IPv6 base85
// variant. Matches pyrekordbox's own deobfuscate() exactly (it calls
// Python's base64.b85decode).
constexpr const char *B85Alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";

std::string b85Decode(const std::string &input)
{
    int decodeTable[256];
    for (int &v : decodeTable) {
        v = -1;
    }
    for (int i = 0; i < 85; ++i) {
        decodeTable[static_cast<unsigned char>(B85Alphabet[i])] = i;
    }

    std::string padded = input;
    size_t padding = (5 - (padded.size() % 5)) % 5;
    padded.append(padding, '~');

    std::string out;
    out.reserve(padded.size() / 5 * 4);
    for (size_t i = 0; i < padded.size(); i += 5) {
        uint32_t acc = 0;
        for (size_t j = 0; j < 5; ++j) {
            int v = decodeTable[static_cast<unsigned char>(padded[i + j])];
            if (v < 0) {
                throw std::runtime_error("onelibrary: bad base85 character while deriving key");
            }
            acc = acc * 85 + static_cast<uint32_t>(v);
        }
        char bytes[4] = {static_cast<char>((acc >> 24) & 0xFF), static_cast<char>((acc >> 16) & 0xFF),
                          static_cast<char>((acc >> 8) & 0xFF), static_cast<char>(acc & 0xFF)};
        out.append(bytes, 4);
    }
    if (padding > 0) {
        out.resize(out.size() - padding);
    }
    return out;
}

}  // namespace

std::string deriveOneLibraryKey()
{
    // Matches pyrekordbox/utils.py's BLOB and pyrekordbox/devicelib_plus/
    // database.py's BLOB constant exactly (cross-checked against both
    // files' source during development).
    static const std::string blob =
        "PN_1dH8$oLJY)16j_RvM6qphWw`476>;C1cWmI#se(PG`j}~xAjlufj?`#0i{;=glh(SkW)y0>n?YEiD`l%t(";
    static const std::string xorKey = "657f48f84c437cc1";

    std::string decoded = b85Decode(blob);

    std::string xored;
    xored.resize(decoded.size());
    for (size_t i = 0; i < decoded.size(); ++i) {
        xored[i] = static_cast<char>(static_cast<unsigned char>(decoded[i]) ^
                                      static_cast<unsigned char>(xorKey[i % xorKey.size()]));
    }

    // Unlike this codebase's compression::decompress() (which requires
    // the caller to already know the exact uncompressed size), zlib's
    // own uncompress() only needs an upper bound and reports the real
    // size back via destLen -- exactly what's needed here, since the
    // obfuscated blob carries no length header. The known key is ~65
    // bytes; 4096 is a deliberately generous ceiling.
    uLongf destLen = 4096;
    std::vector<Bytef> dest(destLen);
    int rc = uncompress(dest.data(), &destLen, reinterpret_cast<const Bytef *>(xored.data()),
                         static_cast<uLong>(xored.size()));
    if (rc != Z_OK) {
        throw std::runtime_error("onelibrary: failed to derive SQLCipher key (zlib inflate error " +
                                  std::to_string(rc) + ")");
    }
    return std::string(reinterpret_cast<char *>(dest.data()), destLen);
}

std::string oneLibraryKeyPragma(const std::string &dbPath)
{
    const std::string passphrase = deriveOneLibraryKey();
    std::array<char, 16> salt{};
    {
        std::ifstream in(pathFromUtf8(dbPath), std::ios::binary);
        if (!in.read(salt.data(), static_cast<std::streamsize>(salt.size()))) {
            return "PRAGMA key = '" + passphrase + "';";
        }
    }
    // A plain SQLite file starts with its format string, not a salt.
    static constexpr char plainHeader[16] = {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
                                             'o', 'r', 'm', 'a', 't', ' ', '3', '\0'};
    if (std::equal(salt.begin(), salt.end(), plainHeader)) {
        return "PRAGMA key = '" + passphrase + "';";
    }
    const std::string saltKey(salt.data(), salt.size());
    static std::mutex mutex;
    static std::map<std::string, std::string> rawKeyBySalt;
    // Held over the derivation too: a second thread opening the same
    // database waits for the first one's key rather than deriving its own.
    const std::lock_guard<std::mutex> lock(mutex);
    auto found = rawKeyBySalt.find(saltKey);
    if (found == rawKeyBySalt.end()) {
        // SQLCipher 4's defaults, which OneLibrary uses unchanged (see
        // deriveOneLibraryKey): PBKDF2-HMAC-SHA512, 256000 rounds, a
        // 32-byte key.
        const std::vector<std::uint8_t> saltBytes(salt.begin(), salt.end());
        const auto key = hashing::pbkdf2HmacSha512(passphrase, saltBytes, 256000, 32);
        static const char *digits = "0123456789ABCDEF";
        std::string hex;
        for (const std::uint8_t b : key) {
            hex += digits[b >> 4];
            hex += digits[b & 15];
        }
        // The salt goes with the key, SQLCipher's 96-digit form. The key
        // is right for this salt only, and the file is read again when the
        // connection opens it: should it be empty or gone by then, the
        // database SQLCipher starts is given this salt rather than a new
        // one, and so still opens with the passphrase, which is all
        // rekordbox and a player have.
        for (const std::uint8_t b : saltBytes) {
            hex += digits[b >> 4];
            hex += digits[b & 15];
        }
        found = rawKeyBySalt.emplace(saltKey, hex).first;
    }
    return "PRAGMA key = \"x'" + found->second + "'\";";
}

}  // namespace seabass::infrastructure::onelibrary
