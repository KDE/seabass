// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Standalone investigation tool: lists every audio file on a stick that
// no catalog references, using the project's own walker, readers and
// decision logic -- so the numbers match what Seabass itself would
// propose, rather than a re-derivation that only looks similar.
//
// Written to check findUnreferencedFiles() and walkAudioFiles() against
// real hardware before either feeds a deletion. See
// docs/unreferenced-file-cleanup-plan.md.
//
// Not part of the CMake build (a one-off investigation tool, like
// tools/catalog_dump.cpp). Build against an existing build tree:
//
//   g++ -std=c++20 -I<repo>/src -I<repo>/third_party/libdjinterop/include \
//       tools/unreferenced_scan.cpp -L<build> -lseabass_core ... -o unreferenced_scan
//
// Reads only; never writes to the stick, and never deletes anything.
#include <cstdio>
#include <iostream>
#include <string>

#include "application/use_cases/find_unreferenced_files.hpp"
#include "infrastructure/cleanup/audio_file_walk.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

using namespace seabass;

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: unreferenced_scan <stick-mount-point> [--list]\n";
        return 1;
    }
    const std::string root = argv[1];
    const bool list = argc > 2 && std::string(argv[2]) == "--list";

    application::CatalogTracks catalogs;
    try {
        infrastructure::rekordbox::KaitaiRekordboxReader rb(root + "/PIONEER");
        catalogs.rekordbox = rb.readAll();
        std::cerr << "rekordbox: " << catalogs.rekordbox->size() << " rows\n";
    } catch (const std::exception &e) {
        std::cerr << "rekordbox: not read (" << e.what() << ")\n";
    }
    try {
        infrastructure::engine::LibdjinteropEngineReader en(root + "/Engine Library");
        catalogs.engine = en.readAll();
        std::cerr << "engine: " << catalogs.engine->size() << " rows\n";
    } catch (const std::exception &e) {
        std::cerr << "engine: not read (" << e.what() << ")\n";
    }

    try {
        infrastructure::onelibrary::OneLibraryReader ol(root + "/PIONEER");
        catalogs.oneLibrary = ol.readAll();
        std::cerr << "onelibrary: " << catalogs.oneLibrary->size() << " rows\n";
    } catch (const std::exception &e) {
        std::cerr << "onelibrary: not read (" << e.what() << ")\n";
    }

    auto walk = infrastructure::cleanup::walkAudioFiles(root + "/Contents", application::CancellationToken());
    std::cerr << "walked: " << walk.files.size() << " audio files in " << walk.directoriesVisited
              << " directories" << (walk.incomplete ? " (INCOMPLETE -- some entries unreadable)" : "") << "\n";

    auto scan = application::findUnreferencedFiles(walk.files, catalogs);
    if (!scan.usable) {
        std::cerr << "no catalog could be read -- refusing to call anything unreferenced\n";
        return 2;
    }

    std::string consulted;
    for (const auto &name : scan.catalogsConsulted) {
        consulted += (consulted.empty() ? "" : ", ") + name;
    }
    unsigned long long bytes = 0;
    for (const auto &f : scan.unreferenced) {
        bytes += f.fileSizeBytes;
        if (list) {
            std::printf("%llu\t%s\n", (unsigned long long)f.fileSizeBytes, f.filePath.c_str());
        }
    }
    std::fprintf(stderr, "checked against: %s\n", consulted.c_str());
    std::fprintf(stderr, "unreferenced: %zu of %zu audio files, %.2f GB\n", scan.unreferenced.size(),
                 scan.audioFilesSeen, double(bytes) / 1e9);
    return 0;
}
