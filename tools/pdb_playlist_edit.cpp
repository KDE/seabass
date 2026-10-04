// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Hardware driver for #62: edits a test stick's export.pdb playlists with
// PdbRowWriter, the code the playlist repair uses, so a player can show
// what it made. Tracks are named by file name, playlists by name.
//
//   pdb_playlist_edit <stick> show
//   pdb_playlist_edit <stick> append <playlist> <file name>...
//   pdb_playlist_edit <stick> remove <playlist> <file name>...
//   pdb_playlist_edit <stick> reorder <playlist> <file name>...   (the new order, every track)
//   pdb_playlist_edit <stick> create <name> [<folder>]           (a playlist, top level or in a folder)
//   pdb_playlist_edit <stick> mkfolder <name>
//   pdb_playlist_edit <stick> delete <playlist or folder>
//
// <stick> is the mount point; it must be a test stick (TESTRIG or
// TESTROWS), so the reference sticks can never be written by mistake. Every edit prints the
// playlists and the playlist pages afterwards.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <kaitai/kaitaistream.h>
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace fs = std::filesystem;
namespace rb = seabass::infrastructure::rekordbox;

namespace
{

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

struct Catalog
{
    std::map<std::string, uint32_t> trackIdByFile;
    std::map<uint32_t, std::string> fileByTrackId;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> entriesByPlaylist;  // entry_index -> track
    std::vector<std::string> pages;
};

Catalog read(const fs::path &pdbFile)
{
    Catalog c;
    const std::string bytes = readFile(pdbFile);
    std::istringstream is(bytes);
    kaitai::kstream ks(&is);
    rekordbox_pdb_t pdb(false, &ks);
    for (const auto &table : *pdb.tables()) {
        const bool tracks = table->type() == rekordbox_pdb_t::PAGE_TYPE_TRACKS;
        const bool entries = table->type() == rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES;
        if (!tracks && !entries) {
            continue;
        }
        rb::forEachDataPage(*table, [&](rekordbox_pdb_t::page_t *page) {
            int present = 0;
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    ++present;
                    if (tracks) {
                        auto *t = static_cast<rekordbox_pdb_t::track_row_t *>(row->body());
                        const std::string name = trimmed(rb::sqlText(t->filename()));
                        c.trackIdByFile[name] = t->id();
                        c.fileByTrackId[t->id()] = name;
                    } else {
                        auto *e = static_cast<rekordbox_pdb_t::playlist_entry_row_t *>(row->body());
                        c.entriesByPlaylist[e->playlist_id()][e->entry_index()] = e->track_id();
                    }
                }
            }
            if (entries) {
                const size_t base = static_cast<size_t>(pdb.len_page()) * page->page_index();
                const auto u16 = [&](size_t o) {
                    return static_cast<unsigned>(static_cast<unsigned char>(bytes[base + o])
                                                 | static_cast<unsigned char>(bytes[base + o + 1]) << 8);
                };
                c.pages.push_back("page " + std::to_string(page->page_index()) + ": " + std::to_string(present)
                                  + " entries, " + std::to_string(u16(24) & 0x1FFF) + " rows written, free "
                                  + std::to_string(u16(28)) + " bytes, next page " + std::to_string(page->next_page()->index()));
            }
        });
    }
    return c;
}

void show(const fs::path &pioneer)
{
    const fs::path pdbFile = pioneer / "rekordbox" / "export.pdb";
    const Catalog c = read(pdbFile);
    for (const auto &[name, id] : rb::rekordboxPlaylistIdsByPath(seabass::pathToUtf8(pioneer))) {
        const auto found = c.entriesByPlaylist.find(id);
        const size_t n = found == c.entriesByPlaylist.end() ? 0 : found->second.size();
        std::cout << name << " (" << n << "):";
        uint32_t expected = 1;
        if (found != c.entriesByPlaylist.end()) {
            for (const auto &[index, track] : found->second) {
                std::cout << (index == expected ? " " : " [gap] ") << index << "=" << c.fileByTrackId.at(track);
                expected = index + 1;
            }
        }
        std::cout << "\n";
    }
    for (const auto &p : c.pages) {
        std::cout << p << "\n";
    }
    std::cout << "file: " << fs::file_size(pdbFile) / 4096 << " pages\n";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: pdb_playlist_edit <stick> show | append <playlist> <file>... | remove <playlist> <file>...\n";
        return 2;
    }
    const fs::path stick = seabass::pathFromUtf8(argv[1]);
    if (stick.filename() != "TESTRIG" && stick.filename() != "TESTROWS") {
        std::cerr << "refusing: only a stick mounted as TESTRIG or TESTROWS is written\n";
        return 2;
    }
    const fs::path pioneer = stick / "PIONEER";
    const std::string verb = argv[2];
    if (verb == "show") {
        show(pioneer);
        return 0;
    }
    const fs::path pdbPath = pioneer / "rekordbox" / "export.pdb";
    if (verb == "create" || verb == "mkfolder" || verb == "delete") {
        if (argc < 4) {
            std::cerr << "usage: see the top of tools/pdb_playlist_edit.cpp\n";
            return 2;
        }
        rb::PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        const auto tree = writer.playlistTree();
        const auto idOf = [&](const std::string &name) -> std::optional<uint32_t> {
            for (const auto &n : tree) {
                if (n.name == name) {
                    return n.id;
                }
            }
            return std::nullopt;
        };
        try {
            if (verb == "delete") {
                const auto id = idOf(argv[3]);
                if (!id) {
                    std::cerr << "no playlist or folder \"" << argv[3] << "\"\n";
                    return 1;
                }
                std::cout << "deleted " << writer.deletePlaylist(*id) << " playlists and folders\n";
            } else {
                uint32_t parent = 0;
                if (verb == "create" && argc > 4) {
                    const auto id = idOf(argv[4]);
                    if (!id) {
                        std::cerr << "no folder \"" << argv[4] << "\"\n";
                        return 1;
                    }
                    parent = *id;
                }
                std::cout << "created id " << writer.createPlaylist(parent, argv[3], verb == "mkfolder") << "\n";
            }
        } catch (const std::exception &e) {
            std::cerr << "refused: " << e.what() << "\n";
            return 1;
        }
        if (!writer.commit()) {
            std::cerr << "commit failed, export.pdb untouched\n";
            return 1;
        }
        show(pioneer);
        return 0;
    }
    if ((verb != "append" && verb != "remove" && verb != "reorder") || argc < 5) {
        std::cerr << "usage: pdb_playlist_edit <stick> show | append <playlist> <file>... | remove <playlist> <file>...\n";
        return 2;
    }
    const auto playlists = rb::rekordboxPlaylistIdsByPath(seabass::pathToUtf8(pioneer));
    const auto playlist = playlists.find(argv[3]);
    if (playlist == playlists.end()) {
        std::cerr << "no playlist \"" << argv[3] << "\"\n";
        return 1;
    }
    const fs::path pdbFile = pioneer / "rekordbox" / "export.pdb";
    const Catalog c = read(pdbFile);
    std::vector<uint32_t> ids;
    for (int i = 4; i < argc; ++i) {
        const auto t = c.trackIdByFile.find(argv[i]);
        if (t == c.trackIdByFile.end()) {
            std::cerr << "no track \"" << argv[i] << "\"\n";
            return 1;
        }
        ids.push_back(t->second);
    }
    rb::PdbRowWriter writer(seabass::pathToUtf8(pdbFile));
    size_t changed = 0;
    try {
        if (verb == "append") {
            changed = writer.appendPlaylistEntries(playlist->second, ids);
        } else if (verb == "remove") {
            changed = writer.removePlaylistEntries(playlist->second, std::set<uint32_t>(ids.begin(), ids.end()));
        } else {
            changed = writer.reorderPlaylist(playlist->second, ids) ? ids.size() : 0;
        }
    } catch (const std::exception &e) {
        std::cerr << "refused: " << e.what() << "\n";
        return 1;
    }
    if (changed == 0) {
        std::cout << "nothing to change\n";
        return 0;
    }
    if (!writer.commit()) {
        std::cerr << "commit failed, export.pdb untouched\n";
        return 1;
    }
    std::cout << verb << ": " << changed << " entries in " << argv[3] << "\n";
    show(pioneer);
    return 0;
}
