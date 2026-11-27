// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Writes an empty analysis ledger onto a scratch stick, as a save writes
// one: write_empty_analysis_ledger <stick root>. For
// cli_analysis_ledger_reset.cmake, which needs a ledger that can be read
// and has no gzip of its own in the CMake the project requires.

#include <iostream>
#include <string>

#include "infrastructure/local/analysis_ledger_file.hpp"
#include "infrastructure/paths/utf8_path.hpp"

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: write_empty_analysis_ledger <stick root>\n";
        return 2;
    }
    std::string error;
    if (!seabass::infrastructure::local::writeAnalysisLedger(seabass::pathFromUtf8(argv[1]), {},
                                                      [](const std::string &) {}, &error)) {
        std::cerr << error << "\n";
        return 1;
    }
    return 0;
}
