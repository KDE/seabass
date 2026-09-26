// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Times the three stages of a catalog read through the GUI's own cache,
// against a real stick: what a page pays for Tracks, then Cues, then
// Full, in that order, each stage on top of the one before. Read-only;
// mount the stick read-only for a cold measurement and remount between
// runs (the page cache is what "cold" means here).
//
//   catalog_stage_timing rekordbox /media/sebas/STICK/PIONEER
//   catalog_stage_timing engine "/media/sebas/STICK/Engine Library"
//   catalog_stage_timing onelibrary /media/sebas/STICK/PIONEER

#include <chrono>
#include <iostream>
#include <string>

#include "application/ports/progress_reporter.hpp"
#include "gui/library_catalog_cache.hpp"

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: catalog_stage_timing <rekordbox|engine|onelibrary> <catalog path>\n";
        return 2;
    }
    const std::string format = argv[1];
    const std::string path = argv[2];
    using seabass::gui::LibraryCatalogCache;
    using Detail = LibraryCatalogCache::Detail;
    auto &cache = LibraryCatalogCache::instance();
    auto &progress = seabass::application::NullProgressReporter::instance();

    const auto stage = [&](const char *name, Detail detail) {
        const auto started = std::chrono::steady_clock::now();
        std::size_t tracks = 0;
        std::size_t withCues = 0;
        std::size_t withSizes = 0;
        try {
            const auto read = cache.tracksFor(format, path, detail, progress);
            tracks = read.size();
            for (const auto &track : read) {
                withCues += track.cues.empty() ? 0 : 1;
                withSizes += track.fileSizeBytes == 0 ? 0 : 1;
            }
        } catch (const std::exception &e) {
            std::cout << name << ": failed: " << e.what() << "\n";
            return;
        }
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        std::cout << name << ": " << took.count() << " ms (" << tracks << " tracks, " << withCues << " with cues, "
                  << withSizes << " with a size)\n";
    };
    stage("Tracks", Detail::Tracks);
    stage("Cues  ", Detail::Cues);
    stage("Full  ", Detail::Full);
    return 0;
}
