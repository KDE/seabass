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

}  // namespace seabass::infrastructure::onelibrary
