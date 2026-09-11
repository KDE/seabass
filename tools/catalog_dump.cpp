// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Standalone investigation tool: dumps every track both catalogs on a
// stick know about, as TSV, using the project's own readers (so the
// numbers match what Seabass itself sees -- not a re-derivation from a
// raw string scan of export.pdb, which does not survive contact with
// real data).
//
// Written to answer one question on real hardware: do rekordbox and
// Engine agree on track length, and can rekordbox's lengths fill in the
// rows where Engine left `length` NULL? See docs/real-data-testing.md.
//
// Not part of the CMake build (a one-off investigation tool, like
// tools/onelibrary_audit.cpp). Build against an existing build tree:
//
//   g++ -std=c++20 -I<repo>/src tools/catalog_dump.cpp \
//       -L<build> -lseabass_core ... -o catalog_dump
//
// Reads only; never writes to the stick.
#include <iostream>
#include <memory>
#include <string>

#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

using namespace seabass;

namespace
{
// TSV must survive titles containing tabs/newlines.
std::string clean(std::string s)
{
    for (auto &c : s) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return s;
}

void dump(const char *format, const std::vector<domain::Track> &tracks)
{
    for (const auto &t : tracks) {
        std::cout << format << '\t' << clean(t.sourceId) << '\t' << clean(t.filePath) << '\t'
                  << clean(t.filename) << '\t' << clean(t.title) << '\t' << clean(t.artist) << '\t'
                  << t.durationSeconds << '\t' << t.bitrate << '\t' << t.fileSizeBytes << '\n';
    }
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: catalog_dump <stick-mount-point>\n";
        return 1;
    }
    const std::string root = argv[1];
    std::cout << "format\tsourceId\tfilePath\tfilename\ttitle\tartist\tduration\tbitrate\tfileBytes\n";

    try {
        infrastructure::rekordbox::KaitaiRekordboxReader rb(root + "/PIONEER");
        dump("rekordbox", rb.readAll());
    } catch (const std::exception &e) {
        std::cerr << "rekordbox: " << e.what() << "\n";
    }
    try {
        infrastructure::engine::LibdjinteropEngineReader en(root + "/Engine Library");
        dump("engine", en.readAll());
    } catch (const std::exception &e) {
        std::cerr << "engine: " << e.what() << "\n";
    }
    return 0;
}
