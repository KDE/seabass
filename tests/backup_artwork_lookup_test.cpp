// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Lifting a lost cover out of a stick backup: by name, which is a
// checksum and therefore exact, and -- when even the name is gone -- by
// asking a backed-up library what art it holds for the same audio file.

#include "infrastructure/stick_backup/backup_artwork_lookup.hpp"

#include <sqlite3.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <span>
#include <system_error>
#include <vector>

#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::stick_backup;

namespace
{

std::string jpeg(const std::string &payload)
{
    return std::string("\xFF\xD8\xFF", 3) + payload;
}

void writeFile(const fs::path &file, const std::string &bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A stick backup holding one Engine library: one artwork file, and a
// database that says which track it belongs to.
void writeBackup(const fs::path &archive, const std::string &artworkName, const std::string &image,
                 const std::string &trackPath, const std::vector<std::uint8_t> &hash)
{
    const fs::path staging = archive.parent_path() / "staging";
    fs::remove_all(staging);
    const fs::path db = staging / "m.db";
    fs::create_directories(staging);
    sqlite3 *handle = nullptr;
    assert(sqlite3_open(seabass::pathToUtf8(db).c_str(), &handle) == SQLITE_OK);
    auto exec = [&](const std::string &sql) {
        assert(sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    exec("CREATE TABLE AlbumArt (id INTEGER PRIMARY KEY, hash BLOB);");
    exec("CREATE TABLE Track (id INTEGER PRIMARY KEY, albumArtId INTEGER, path TEXT);");
    sqlite3_stmt *insert = nullptr;
    assert(sqlite3_prepare_v2(handle, "INSERT INTO AlbumArt (id, hash) VALUES (1, ?);", -1, &insert, nullptr)
           == SQLITE_OK);
    sqlite3_bind_blob(insert, 1, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
    assert(sqlite3_step(insert) == SQLITE_DONE);
    sqlite3_finalize(insert);
    exec("INSERT INTO Track (id, albumArtId, path) VALUES (1, 1, '../" + trackPath + "');");
    sqlite3_close(handle);

    const std::string dbBytes = [&] {
        std::ifstream in(db, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();

    std::error_code removeEc;
    fs::remove(archive, removeEc);
    PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadWrite);
    Zip64Writer writer(file, {});
    const auto bytesOf = [](const std::string &text) {
        return std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size());
    };
    writer.addFileFromMemory("Engine Library/Artwork/" + artworkName + ".jpg", 1'700'000'000, bytesOf(image));
    writer.addFileFromMemory("Engine Library/Database2/m.db", 1'700'000'000, bytesOf(dbBytes));
    // The lookup never reads the manifest, but an archive has one.
    writer.finish("seabass-stick-manifest\t1\tid\tOLDSTICK\tcomplete\t1700000000\t\t0\n",
                  std::string(ManifestEntryName), 1'700'000'000);
    fs::remove_all(staging);
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_backup_artwork_lookup_test";
    fs::remove_all(root);
    fs::create_directories(root);

    const std::vector<std::uint8_t> hash(20, 0x5A);
    const std::string name = seabass::infrastructure::engine::artworkFileName(hash);
    const std::string image = jpeg("A-COVER-FROM-A-BACKUP");
    const fs::path archive = root / "OLDSTICK.zip";
    writeBackup(archive, name, image, "Contents/Artist/Album/song.mp3", hash);

    // 1. By name: the file the database is asking for, proven by the name
    //    being the hash of the bytes.
    {
        BackupArtworkLookup lookup({archive});
        assert(lookup.has(name));
        assert(lookup.find(name) == image);
        assert(lookup.find("nothing-by-this-name").empty());
        std::cout << "case 1 (a lost artwork file comes back out of a backup by name) OK\n";
    }

    // 2. By track: the row lost its hash, so there is no name to ask for.
    //    A backed-up library that knows the same audio file answers with
    //    whatever cover it kept for it.
    {
        BackupArtworkLookup lookup({archive});
        assert(lookup.findForTrack("Contents/Artist/Album/song.mp3") == image);
        assert(lookup.findForTrack("Contents/Artist/Album/not-in-any-backup.mp3").empty());
        assert(lookup.findForTrack("").empty());
        std::cout << "case 2 (a backed-up library answers for the same track when the hash is gone) OK\n";
    }

    // 3. A backup that is not there, or not a readable archive, is one
    //    fewer place to look and never an error.
    {
        writeFile(root / "TORN.zip", "not a zip at all");
        BackupArtworkLookup lookup({root / "missing.zip", root / "TORN.zip", archive});
        assert(lookup.find(name) == image);
        std::cout << "case 3 (an unreadable backup is skipped, not fatal) OK\n";
    }

    fs::remove_all(root);
    std::cout << "backup_artwork_lookup_test: all cases passed\n";
    return 0;
}
