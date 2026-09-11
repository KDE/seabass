// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Manual verification tool: reads a real ANLZ file at the raw-section
// level and rewrites it unmodified, to prove the section-boundary walk in
// AnlzFile is exact (no gaps, no overlaps, no misread lengths) before any
// code attempts to actually change a section's content. Not built by the
// main CMake project. Usage:
//
//   g++ -std=c++23 -I src \
//       src/infrastructure/rekordbox/anlz_file.cpp \
//       tests/fixtures/debug_anlz_roundtrip.cpp -o /tmp/roundtrip
//   /tmp/roundtrip <path/to/ANLZ0000.EXT> <output-path>
//   cmp <path/to/ANLZ0000.EXT> <output-path> && echo IDENTICAL

#include <iostream>

#include "infrastructure/rekordbox/anlz_file.hpp"

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: roundtrip <input.EXT> <output-path>\n";
        return 1;
    }
    auto file = seabass::infrastructure::rekordbox::AnlzFile::readRaw(argv[1]);
    std::cout << "sections: " << file.sections.size() << "\n";
    for (const auto &section : file.sections) {
        char tag[5] = {static_cast<char>((section.fourcc >> 24) & 0xFF), static_cast<char>((section.fourcc >> 16) & 0xFF),
                        static_cast<char>((section.fourcc >> 8) & 0xFF), static_cast<char>(section.fourcc & 0xFF), 0};
        std::cout << "  " << tag << "  " << section.rawBytes.size() << " bytes\n";
    }
    file.writeRaw(argv[2]);
    return 0;
}
