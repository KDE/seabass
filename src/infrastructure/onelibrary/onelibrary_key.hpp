// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::infrastructure::onelibrary
{

// The SQLCipher passphrase for OneLibrary/Device Library Plus's
// exportLibrary.db. This is NOT a secret Seabass is choosing to store
// insecurely -- Rekordbox itself obfuscates the same fixed, universal key
// (identical across every installation and every exported device, not
// derived from any license or machine identity) inside its own binary
// via base85 + a cycling XOR + zlib, purely to avoid it appearing as a
// plaintext string in a binary/string-table scan. Reversing that
// obfuscation is standard practice among the existing open-source
// Rekordbox-format tooling (e.g. pyrekordbox) that this implementation
// was cross-checked against; see docs/onelibrary-format.md.
//
// Verified during development: this exact derivation, decrypting a real
// exportLibrary.db copied from a real stick, opens cleanly with
// SQLCipher 4's own compiled-in defaults and no other PRAGMA overrides.
std::string deriveOneLibraryKey();

// The PRAGMA that keys a connection to the OneLibrary database at
// `dbPath`, to exec right after opening it. SQLCipher 4 stretches the
// passphrase with 256,000 rounds of PBKDF2-HMAC-SHA512 over the file's
// 16-byte salt (its first 16 bytes) on every open: about 115 ms, paid
// again by every reader, writer and check of the same file. The key it
// arrives at depends on the passphrase and the salt alone, so it is
// derived here once per salt for the life of the process and handed to
// SQLCipher as a raw key, which it uses as is. A file with no salt to
// read (missing, shorter than 16 bytes, or a plain SQLite file) gets the
// passphrase, as before.
std::string oneLibraryKeyPragma(const std::string &dbPath);

}  // namespace seabass::infrastructure::onelibrary
