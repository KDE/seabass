// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Hardware driver for #62: edits a test stick's playlists with the code the
// app uses (PdbRowWriter for export.pdb, OneLibraryCueWriter for
// exportLibrary.db), both halves by default, so a player and rekordbox can
// show what it made. Tracks are named by file name, playlists by name.
// --pdb-only or --onelibrary-only before the verb edits one half.
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
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace fs = std::filesystem;
namespace rb = seabass::infrastructure::rekordbox;
namespace ol = seabass::infrastructure::onelibrary;

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
    std::map<std::string, std::string> pathByFile;  // "/Contents/..." as export.pdb spells it
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
                        c.pathByFile[name] = trimmed(rb::sqlText(t->file_path()));
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

void showOneLibrary(const fs::path &pioneer)
{
    ol::OneLibraryCueWriter w(seabass::pathToUtf8(pioneer));
    const auto tree = w.playlistTree();
    std::map<int64_t, ol::OneLibraryCueWriter::PlaylistNode> byId;
    for (const auto &n : tree) {
        byId[n.id] = n;
    }
    std::map<std::pair<int64_t, int64_t>, std::string> ordered;
    for (const auto &n : tree) {
        std::string path = n.name;
        for (int64_t p = n.parentId; p != 0; p = byId.at(p).parentId) {
            path = byId.at(p).name + "/" + path;
        }
        std::string line = "onelibrary " + path + (n.isFolder ? " (folder)" : " (" + std::to_string(w.playlistContent(path).size()) + ")");
        ordered[{n.parentId, n.sequenceNo}] = line;
    }
    for (const auto &[key, line] : ordered) {
        std::cout << line << "\n";
    }
}

int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    bool pdb = true;
    bool onelibrary = true;
    for (auto it = args.begin(); it != args.end();) {
        if (*it == "--pdb-only") {
            onelibrary = false;
            it = args.erase(it);
        } else if (*it == "--onelibrary-only") {
            pdb = false;
            it = args.erase(it);
        } else {
            ++it;
        }
    }
    if (args.size() < 2) {
        std::cerr << "usage: see the top of tools/pdb_playlist_edit.cpp\n";
        return 2;
    }
    const fs::path stick = seabass::pathFromUtf8(args[0]);
    if (stick.filename() != "TESTRIG" && stick.filename() != "TESTROWS") {
        std::cerr << "refusing: only a stick mounted as TESTRIG or TESTROWS is written\n";
        return 2;
    }
    const fs::path pioneer = stick / "PIONEER";
    const fs::path pdbFile = pioneer / "rekordbox" / "export.pdb";
    const std::string verb = args[1];
    const auto showBoth = [&] {
        show(pioneer);
        showOneLibrary(pioneer);
    };
    if (verb == "show") {
        showBoth();
        return 0;
    }
    const bool treeVerb = verb == "create" || verb == "mkfolder" || verb == "delete";
    const bool entryVerb = verb == "append" || verb == "remove" || verb == "reorder";
    if ((!treeVerb && !entryVerb) || args.size() < (treeVerb ? 3u : 4u)) {
        std::cerr << "usage: see the top of tools/pdb_playlist_edit.cpp\n";
        return 2;
    }
    const std::string name = args[2];
    const std::string folder = verb == "create" && args.size() > 3 ? args[3] : "";
    const Catalog c = read(pdbFile);
    std::vector<uint32_t> ids;
    std::vector<std::string> files;
    if (entryVerb) {
        for (size_t i = 3; i < args.size(); ++i) {
            const auto t = c.trackIdByFile.find(args[i]);
            if (t == c.trackIdByFile.end()) {
                std::cerr << "no track \"" << args[i] << "\"\n";
                return 1;
            }
            ids.push_back(t->second);
            files.push_back(seabass::pathToUtf8(stick) + c.pathByFile.at(args[i]));
        }
    }
    try {
        if (pdb) {
            rb::PdbRowWriter writer(seabass::pathToUtf8(pdbFile));
            const auto tree = writer.playlistTree();
            const auto idOf = [&](const std::string &n) -> std::optional<uint32_t> {
                for (const auto &node : tree) {
                    if (node.name == n) {
                        return node.id;
                    }
                }
                return std::nullopt;
            };
            size_t changed = 0;
            if (verb == "delete") {
                const auto id = idOf(name);
                changed = id ? writer.deletePlaylist(*id) : 0;
            } else if (treeVerb) {
                uint32_t parent = 0;
                if (!folder.empty()) {
                    const auto id = idOf(folder);
                    if (!id) {
                        throw std::invalid_argument("no folder \"" + folder + "\"");
                    }
                    parent = *id;
                }
                writer.createPlaylist(parent, name, verb == "mkfolder");
                changed = 1;
            } else {
                const auto playlists = rb::rekordboxPlaylistIdsByPath(seabass::pathToUtf8(pioneer));
                const auto playlist = playlists.find(name);
                if (playlist == playlists.end()) {
                    throw std::invalid_argument("export.pdb has no playlist \"" + name + "\"");
                }
                if (verb == "append") {
                    changed = writer.appendPlaylistEntries(playlist->second, ids);
                } else if (verb == "remove") {
                    changed = writer.removePlaylistEntries(playlist->second, std::set<uint32_t>(ids.begin(), ids.end()));
                } else {
                    changed = writer.reorderPlaylist(playlist->second, ids) ? ids.size() : 0;
                }
            }
            if (changed > 0 && !writer.commit()) {
                std::cerr << "export.pdb: commit failed, untouched\n";
                return 1;
            }
            std::cout << "export.pdb: " << verb << " " << name << ": " << changed << "\n";
        }
        if (onelibrary) {
            ol::OneLibraryCueWriter w(seabass::pathToUtf8(pioneer));
            size_t changed = 0;
            if (verb == "delete") {
                changed = w.deletePlaylist(name);
            } else if (treeVerb) {
                w.createPlaylist(folder, name, verb == "mkfolder");
                changed = 1;
            } else if (verb == "append") {
                for (const auto &f : files) {
                    changed += w.addToPlaylist(name, f) ? 1 : 0;
                }
            } else if (verb == "remove") {
                for (const auto &f : files) {
                    changed += w.removeFromPlaylist(name, f) ? 1 : 0;
                }
            } else {
                changed = w.reorderPlaylist(name, files) ? files.size() : 0;
            }
            std::cout << "onelibrary: " << verb << " " << name << ": " << changed << "\n";
        }
    } catch (const std::exception &e) {
        std::cerr << "refused: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
