// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Checks an anonymized export against the promise its manifest makes.
//
// Usage:
//   verify_anonymized_export <export-directory>
//
// Takes the staging directory, not the zip: the export flow runs this
// before writing the zip so a failure can refuse to produce the file at
// all. Pointing it at an already-unpacked export works the same way.
//
// Exit status is 0 when the export is clean and 1 when it is not, so it
// can gate anything.

#include <iostream>
#include <string>

#include "infrastructure/anonymization_verifier.hpp"

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: verify_anonymized_export <export-directory>\n";
        return 2;
    }
    const auto result = seabass::infrastructure::verifyAnonymizedExport(argv[1]);
    std::cout << result.describe();
    return result.ok ? 0 : 1;
}
