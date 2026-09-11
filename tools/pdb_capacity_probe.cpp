// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Standalone investigation tool: reports how much room an export.pdb has
// left, how many of its rows are deleted rather than absent, and how far
// it has drifted from the OneLibrary sitting beside it.
//
// Written to answer one question: what would it take for Seabass to add a
// track row to export.pdb, which it cannot do today? Three things had to
// be measured rather than reasoned about, and two of them contradicted
// what the format spec suggested. See
// docs/library-health-format-divergence.md for what the numbers decided.
//
// Not part of the CMake build (a one-off investigation tool, like
// tools/catalog_dump.cpp). Build against an existing build tree:
//
//   g++ -std=c++20 -I<repo>/src \
//       -I<repo>/third_party/kaitai_struct_cpp_stl_runtime \
//       tools/pdb_capacity_probe.cpp -L<build> -lseabass_core \
//       -lrekordbox_format -lkaitai_cpp_stl_runtime -lz -ldl \
//       -o pdb_capacity_probe
//   ./pdb_capacity_probe <stick>/PIONEER/rekordbox/export.pdb <stick>/PIONEER
//
// Reads only; never writes.
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

#include <kaitai/kaitaistream.h>
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "domain/track.hpp"
#include <filesystem>

// export.pdb space-pads its fixed-length string fields, so a raw read
// never compares equal to the same path from anywhere else. This is the
// same trim toContentPath() does.
static std::string trimmed(const std::string &in)
{
    std::size_t end = in.size();
    while (end > 0 && (in[end - 1] == ' ' || in[end - 1] == '\t')) {
        --end;
    }
    return in.substr(0, end);
}

static std::string pdbPath(rekordbox_pdb_t::device_sql_string_t *s)
{
    return trimmed(seabass::infrastructure::rekordbox::sqlText(s));
}

int main(int argc, char **argv)
{
    std::ifstream in(argv[1], std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string buffer = ss.str();
    std::istringstream is(buffer);
    kaitai::kstream ks(&is);
    rekordbox_pdb_t pdb(false, &ks);

    std::cout << "file size:                " << buffer.size() << " bytes\n";
    std::cout << "page size:                " << pdb.len_page() << "\n";
    std::cout << "pages in file:            " << (buffer.size() / pdb.len_page()) << "\n";
    std::cout << "next_unused_page:         " << pdb.next_unused_page() << "\n";
    std::cout << "num_tables:               " << pdb.num_tables() << "\n";
    std::cout << "sequence:                 " << pdb.sequence() << "\n";
    for (const auto &table : *pdb.tables()) {
        std::cout << "  table type=" << int(table->type())
                  << " first=" << table->first_page()->index()
                  << " last=" << table->last_page()->index()
                  << " empty_candidate=" << table->empty_candidate() << "\n";
    }

    long long lastId = 0, idOutOfOrder = 0, maxId = 0;
    long long pages = 0, live = 0, deleted = 0, deletedBad = 0;
    long long freeTotal = 0, usedTotal = 0, maxFree = 0, pagesFittingARow = 0;
    long long rowBytesTotal = 0;
    std::set<std::string> livePaths, deletedPaths;

    for (const auto &table : *pdb.tables()) {
        if (table->type() != rekordbox_pdb_t::PAGE_TYPE_TRACKS) {
            continue;
        }
        seabass::infrastructure::rekordbox::forEachDataPage(*table, [&](rekordbox_pdb_t::page_t *page) {
            ++pages;
            freeTotal += page->free_size();
            usedTotal += page->used_size();
            if (page->free_size() > maxFree) {
                maxFree = page->free_size();
            }
            const int allocated = static_cast<int>(page->num_row_offsets());
            int seen = 0;
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (seen >= allocated) {
                        break;
                    }
                    ++seen;
                    if (row->present()) {
                        ++live;
                        auto *t = static_cast<rekordbox_pdb_t::track_row_t *>(row->body());
                        livePaths.insert(pdbPath(t->file_path()));
                        // Does the hardware need rows in id order? If ids
                        // climb monotonically through the page chain, the
                        // file is sorted and an appended row would land in
                        // the wrong place.
                        if (t->id() < lastId) {
                            ++idOutOfOrder;
                        }
                        lastId = t->id();
                        if (t->id() > maxId) {
                            maxId = t->id();
                        }
                        continue;
                    }
                    ++deleted;
                    try {
                        auto *io = row->_io();
                        io->seek(row->row_base());
                        rekordbox_pdb_t::track_row_t t(io, row.get(), &pdb);
                        const std::string path = pdbPath(t.file_path());
                        if (path.empty() || path[0] != '/') {
                            ++deletedBad;
                        } else {
                            deletedPaths.insert(path);
                        }
                    } catch (const std::exception &) {
                        ++deletedBad;
                    }
                }
                if (seen >= allocated) {
                    break;
                }
            }
        });
    }
    if (live > 0) {
        rowBytesTotal = usedTotal;
    }

    long long resurrectable = 0;
    for (const auto &p : deletedPaths) {
        if (!livePaths.count(p)) {
            ++resurrectable;
        }
    }

    const double meanRow = live > 0 ? double(rowBytesTotal) / double(live + deleted) : 0.0;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != rekordbox_pdb_t::PAGE_TYPE_TRACKS) {
            continue;
        }
        seabass::infrastructure::rekordbox::forEachDataPage(*table, [&](rekordbox_pdb_t::page_t *page) {
            if (page->free_size() >= meanRow) {
                ++pagesFittingARow;
            }
        });
    }

    std::cout << "track ids: max=" << maxId << " out-of-order steps through the page chain=" << idOutOfOrder << "\n";
    std::cout << "track pages:              " << pages << "\n";
    std::cout << "live rows:                " << live << " (" << livePaths.size() << " distinct paths)\n";
    std::cout << "deleted rows:             " << deleted << "\n";
    std::cout << "  bodies still readable:  " << (deleted - deletedBad) << " (" << deletedPaths.size() << " distinct paths)\n";
    std::cout << "  unreadable/overwritten: " << deletedBad << "\n";
    std::cout << "  naming a path no live row has: " << resurrectable << "\n";
    std::cout << "heap used:                " << usedTotal << " bytes\n";
    std::cout << "heap free:                " << freeTotal << " bytes (mean " << (freeTotal / pages) << "/page, max " << maxFree << ")\n";
    std::cout << "mean bytes per row:       " << meanRow << "\n";
    std::cout << "pages with room for one more row: " << pagesFittingARow << " of " << pages << "\n";

    // The whole point: are the tracks OneLibrary has and export.pdb does
    // not sitting in the pdb as deleted rows?
    if (argc > 2) {
        seabass::infrastructure::onelibrary::OneLibraryReader ol(argv[2]);
        const auto tracks = ol.readAll();
        const std::filesystem::path stickRoot = std::filesystem::path(argv[2]).parent_path();
        std::set<std::string> olPaths;
        for (const auto &t : tracks) {
            if (t.filePath.empty()) {
                continue;
            }
            std::string rel = std::filesystem::relative(t.filePath, stickRoot).generic_string();
            olPaths.insert("/" + rel);
        }
        long long onlyInOl = 0, coveredByDeleted = 0;
        for (const auto &p : olPaths) {
            if (livePaths.count(p)) {
                continue;
            }
            ++onlyInOl;
            if (deletedPaths.count(p)) {
                ++coveredByDeleted;
            }
        }
        std::cout << "\nOneLibrary tracks:        " << tracks.size() << " (" << olPaths.size() << " resolvable paths)\n";
        std::cout << "  present in export.pdb:  " << (olPaths.size() - onlyInOl) << "\n";
        std::cout << "  missing from export.pdb:" << onlyInOl << "\n";
        std::cout << "    of those, sitting there as a deleted row: " << coveredByDeleted << "\n";
    }

    // The paths themselves are library content, so they are only written
    // out when a destination is named -- never to a default location.
    if (argc > 3) {
        std::ofstream dump(std::string(argv[3]) + "/pdb-deleted-paths.txt");
        for (const auto &p : deletedPaths) {
            if (!livePaths.count(p)) {
                dump << p << "\n";
            }
        }
    }
    return 0;
}
